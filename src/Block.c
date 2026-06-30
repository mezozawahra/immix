#include <stdio.h>
#include <memory.h>
#include "Block.h"
#include "Object.h"
#include "Log.h"
#include "Marker.h"
#include "concurrent/GlobalBlockAllocator.h"

extern int __object_array_id;

#define NO_RECYCLABLE_LINE -1

INLINE void Block_recycleUnmarkedBlock(GlobalBlockAllocator *globalAllocator,
                                       BlockHeader *blockHeader) {
    memset(blockHeader, 0, LINE_SIZE);
    GlobalBlockAllocator_AddFreeBlock(globalAllocator, blockHeader);
    Block_SetFlag(blockHeader, block_free);
}

INLINE void Block_recycleMarkedLine(BlockHeader *blockHeader,
                                    LineHeader *lineHeader, int lineIndex) {
    Line_Unmark(lineHeader);
    if (Line_ContainsObject(lineHeader)) {
        Object *object = Line_GetFirstObject(lineHeader);
        word_t *lineEnd =
            Block_GetLineAddress(blockHeader, lineIndex) + WORDS_IN_LINE;
        while (object != NULL && (word_t *)object < lineEnd) {
            ObjectHeader *objectHeader = &object->header;
            if (Object_IsMarked(objectHeader)) {
                Object_SetAllocated(objectHeader);
            } else {
                Object_SetFree(objectHeader);
            }
            object = Object_NextObject(object);
        }
    }
}

/**
 * Recycles a block and reports the result back to the shared
 * GlobalBlockAllocator. `freedInBlock` is accumulated locally and reported
 * once per block (one lock/unlock) rather than once per recycled line.
 */
void Block_Recycle(GlobalBlockAllocator *globalAllocator,
                   BlockHeader *blockHeader) {

    if (!Block_IsMarked(blockHeader)) {
        Block_recycleUnmarkedBlock(globalAllocator, blockHeader);
        GlobalBlockAllocator_AddFreeMemory(globalAllocator, BLOCK_TOTAL_SIZE);
        return;
    }

    assert(Block_IsMarked(blockHeader));
    Block_Unmark(blockHeader);
    int16_t lineIndex = 0;
    int lastRecyclable = NO_RECYCLABLE_LINE;
    size_t freedInBlock = 0;

    while (lineIndex < LINE_COUNT) {
        LineHeader *lineHeader = Block_GetLineHeader(blockHeader, lineIndex);
        if (Line_IsMarked(lineHeader)) {
            Block_recycleMarkedLine(blockHeader, lineHeader, lineIndex);
            lineIndex++;
        } else {
            if (lastRecyclable == NO_RECYCLABLE_LINE) {
                blockHeader->header.first = lineIndex;
            } else {
                Block_GetFreeLineHeader(blockHeader, lastRecyclable)->next =
                    lineIndex;
            }
            lastRecyclable = lineIndex;
            lineIndex++;
            Line_SetEmpty(lineHeader);
            freedInBlock += LINE_SIZE;
            uint8_t size = 1;
            while (lineIndex < LINE_COUNT &&
                   !Line_IsMarked(lineHeader = Block_GetLineHeader(
                                      blockHeader, lineIndex))) {
                size++;
                lineIndex++;
                Line_SetEmpty(lineHeader);
                freedInBlock += LINE_SIZE;
            }
            Block_GetFreeLineHeader(blockHeader, lastRecyclable)->size = size;
        }
    }

    if (freedInBlock > 0) {
        GlobalBlockAllocator_AddFreeMemory(globalAllocator, freedInBlock);
    }

    if (lastRecyclable == NO_RECYCLABLE_LINE) {
        Block_SetFlag(blockHeader, block_unavailable);
    } else {
        Block_GetFreeLineHeader(blockHeader, lastRecyclable)->next =
            LAST_HOLE;
        Block_SetFlag(blockHeader, block_recyclable);
        GlobalBlockAllocator_AddRecycledBlock(globalAllocator, blockHeader);

        assert(blockHeader->header.first != NO_RECYCLABLE_LINE);
    }
}

void Block_Print(BlockHeader *block) {
    printf("%p ", block);
    if (Block_IsFree(block)) {
        printf("FREE\n");
    } else if (Block_IsUnavailable(block)) {
        printf("UNAVAILABLE\n");
    } else {
        int lineIndex = block->header.first;
        while (lineIndex != LAST_HOLE) {
            FreeLineHeader *freeLineHeader =
                Block_GetFreeLineHeader(block, lineIndex);
            printf("[index: %d, size: %d] -> ", lineIndex,
                   freeLineHeader->size);
            lineIndex = freeLineHeader->next;
        }
        printf("\n");
    }
    fflush(stdout);
}