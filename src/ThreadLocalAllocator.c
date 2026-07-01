#include <stdlib.h>
#include "ThreadLocalAllocator.h"
#include "Line.h"
#include "Block.h"
#include <stdio.h>
#include <memory.h>
#include "concurrent/MutatorSync.h"

BlockHeader *ThreadLocalAllocator_getNextBlock(ThreadLocalAllocator *allocator);
bool ThreadLocalAllocator_getNextLine(ThreadLocalAllocator *allocator);

ThreadLocalAllocator *ThreadLocalAllocator_Create(GlobalBlockAllocator *globalAllocator) {
    ThreadLocalAllocator *allocator = malloc(sizeof(ThreadLocalAllocator));
    allocator->globalAllocator = globalAllocator;

    ThreadLocalAllocator_InitCursors(allocator);

    return allocator;
}

bool ThreadLocalAllocator_CanInitCursors(ThreadLocalAllocator *allocator) {
    return GlobalBlockAllocator_CanInitCursors(allocator->globalAllocator,
                                               MutatorSync_ThreadCount());
}

void ThreadLocalAllocator_InitCursors(ThreadLocalAllocator *allocator) {
    allocator->block = NULL;
    allocator->cursor = NULL;
    allocator->limit = NULL;

    ThreadLocalAllocator_getNextLine(allocator);

    BlockHeader *largeHeader =
        GlobalBlockAllocator_GetFreeBlock(allocator->globalAllocator);
    assert(largeHeader != NULL);
    allocator->largeBlock = largeHeader;
    allocator->largeCursor = Block_GetFirstWord(largeHeader);
    allocator->largeLimit = Block_GetBlockEnd(largeHeader);
}

bool ThreadLocalAllocator_ShouldGrow(ThreadLocalAllocator *allocator) {
    return GlobalBlockAllocator_ShouldGrow(allocator->globalAllocator);
}

uintptr_t *ThreadLocalAllocator_overflowAllocation(ThreadLocalAllocator *allocator, size_t size) {
    uintptr_t *start = allocator->largeCursor;
    uintptr_t *end = (uintptr_t *)((uint8_t *)start + size);

    if (end > allocator->largeLimit) {
        BlockHeader *block =
            GlobalBlockAllocator_GetFreeBlock(allocator->globalAllocator);
        if (block == NULL) {
            return NULL;
        }
        allocator->largeBlock = block;
        allocator->largeCursor = Block_GetFirstWord(block);
        allocator->largeLimit = Block_GetBlockEnd(block);
        return ThreadLocalAllocator_overflowAllocation(allocator, size);
    }

    if (end == allocator->largeLimit) {
        memset(start, 0, size);
    } else {
        memset(start, 0, size + OBJ_ALIGN);
    }

    allocator->largeCursor = end;
    Line_Update(allocator->largeBlock, start);
    return start;
}

INLINE uintptr_t *ThreadLocalAllocator_Alloc(ThreadLocalAllocator *allocator, size_t size) {
    uintptr_t *start = allocator->cursor;
    uintptr_t *end = (uintptr_t *)((uint8_t *)start + size);

    if (end > allocator->limit) {
        if (size > LINE_SIZE) {
            return ThreadLocalAllocator_overflowAllocation(allocator, size);
        } else {
            if (ThreadLocalAllocator_getNextLine(allocator)) {
                return ThreadLocalAllocator_Alloc(allocator, size);
            }
            return NULL;
        }
    }

    if (end == allocator->limit) {
        memset(start, 0, size);
    } else {
        memset(start, 0, size + OBJ_ALIGN);
    }

    allocator->cursor = end;
    Line_Update(allocator->block, start);
    return start;
}

bool ThreadLocalAllocator_nextLineRecycled(ThreadLocalAllocator *allocator) {
    BlockHeader *block = Block_GetBlockHeader(allocator->cursor - OBJ_ALIGN);
    assert(Block_IsRecyclable(block));

    int16_t lineIndex = block->header.first;
    if (lineIndex == LAST_HOLE) {
        allocator->cursor = NULL;
        return ThreadLocalAllocator_getNextLine(allocator);
    }

    uintptr_t *line = Block_GetLineAddress(block, lineIndex);
    allocator->cursor = line;
    FreeLineHeader *lineHeader = (FreeLineHeader *)line;
    block->header.first = lineHeader->next;
    uint16_t size = lineHeader->size;
    allocator->limit = line + (size * SLOTS_IN_LINE);
    return true;
}

void ThreadLocalAllocator_firstLineNewBlock(ThreadLocalAllocator *allocator, BlockHeader *block) {
    allocator->block = block;

    if (Block_IsFree(block)) {
        allocator->cursor = Block_GetFirstWord(block);
        allocator->limit = Block_GetBlockEnd(block);
    } else {
        assert(Block_IsRecyclable(block));
        int16_t lineIndex = block->header.first;
        assert(lineIndex < LINE_COUNT);
        uintptr_t *line = Block_GetLineAddress(block, lineIndex);

        allocator->cursor = line;
        FreeLineHeader *lineHeader = (FreeLineHeader *)line;
        block->header.first = lineHeader->next;
        uint16_t size = lineHeader->size;
        assert(size > 0);
        allocator->limit = line + (size * SLOTS_IN_LINE);
    }
}

bool ThreadLocalAllocator_getNextLine(ThreadLocalAllocator *allocator) {
    if (allocator->cursor == NULL ||
        Block_IsFree(Block_GetBlockHeader(allocator->cursor - OBJ_ALIGN))) {
        BlockHeader *block = ThreadLocalAllocator_getNextBlock(allocator);
        if (block == NULL) {
            return false;
        }
        ThreadLocalAllocator_firstLineNewBlock(allocator, block);
        return true;
    } else {
        return ThreadLocalAllocator_nextLineRecycled(allocator);
    }
}

BlockHeader *ThreadLocalAllocator_getNextBlock(ThreadLocalAllocator *allocator) {
    return GlobalBlockAllocator_GetBlock(allocator->globalAllocator);
}