#include <stddef.h>
#include <stdio.h>
#include "Object.h"
#include "headers/BlockHeader.h"
#include "Line.h"
#include "Log.h"
#include "utils/MathUtils.h"

Object *Object_NextLargeObject(Object *object) {
    size_t size = Object_ChunkSize(object);
    assert(size != 0);
    return (Object *)((ubyte_t *)object + size);
}

Object *Object_NextObject(Object *object) {
    size_t size = Object_Size(&object->header);
    assert(size < LARGE_BLOCK_SIZE);
    if (size == 0) {
        return NULL;
    }
    Object *next = (Object *)((ubyte_t *)object + size);
    assert(Block_GetBlockHeader(next) ==
               Block_GetBlockHeader(object) ||
           (ubyte_t *)Block_GetBlockHeader(next) ==
               (ubyte_t *)Block_GetBlockHeader(object) +
                   BLOCK_TOTAL_SIZE);
    return next;
}

static inline bool isWordAligned(void *addr) {
    return ((uintptr_t)addr & OBJ_ALIGN_INVERSE_MASK) == (uintptr_t)addr;
}

Object *Object_getInLine(BlockHeader *blockHeader, int lineIndex,
                         void *addr) {
    assert(Line_ContainsObject(Block_GetLineHeader(blockHeader, lineIndex)));

    Object *current =
        Line_GetFirstObject(Block_GetLineHeader(blockHeader, lineIndex));
    Object *next = Object_NextObject(current);

    uintptr_t *lineEnd =
        Block_GetLineAddress(blockHeader, lineIndex) + SLOTS_IN_LINE;

    while (next != NULL && (uintptr_t *)next < lineEnd && (uintptr_t *)next <= word) {
        current = next;
        next = Object_NextObject(next);
    }

    if (Object_IsAllocated(&current->header) && word >= (uintptr_t *)current &&
        word < (uintptr_t *)next) {
#ifdef DEBUG_PRINT
        if ((uintptr_t *)current != word) {
            printf("inner pointer: %p object: %p\n", word, current);
            fflush(stdout);
        }
#endif
        return current;
    } else {
#ifdef DEBUG_PRINT
        printf("ignoring %p\n", word);
        fflush(stdout);
#endif
        return NULL;
    }
}

Object *Object_GetObject(void *addr) {
    BlockHeader *blockHeader = Block_GetBlockHeader(addr);

    // Check if the word points on the block header
    if (addr < Block_GetFirstWord(blockHeader)) {
#ifdef DEBUG_PRINT
        printf("Points on block header %p\n", word);
        fflush(stdout);
#endif
        return NULL;
    }

    if (!isWordAligned(addr)) {
#ifdef DEBUG_PRINT
        printf("Word not aligned: %p aligning to %p\n", word,
               (uintptr_t *)((uintptr_t)word & OBJ_ALIGN_INVERSE_MASK));
        fflush(stdout);
#endif
        addr = (uintptr_t *)((uintptr_t)addr & OBJ_ALIGN_INVERSE_MASK);
    }

    int lineIndex = Block_GetLineIndexFromWord(blockHeader, addr);
    while (lineIndex > 0 &&
           !Line_ContainsObject(Block_GetLineHeader(blockHeader, lineIndex))) {
        lineIndex--;
    }

    if (Line_ContainsObject(Block_GetLineHeader(blockHeader, lineIndex))) {
        return Object_getInLine(blockHeader, lineIndex, addr);
    } else {
#ifdef DEBUG_PRINT
        printf("Word points to empty line %p\n", word);
        fflush(stdout);
#endif
        return NULL;
    }
}

Object *Object_getLargeInnerPointer(LargeAllocator *allocator, uintptr_t *word) {
    uintptr_t *current = (uintptr_t *)((uintptr_t)word & LARGE_BLOCK_MASK);

    while (!Bitmap_GetBit(allocator->bitmap, (ubyte_t *)current)) {
        current = (ptr_t *)((ubyte_t *)current - LARGE_BLOCK_SIZE);
    }

    Object *object = (Object *)current;
    if ((ubyte_t *)word < (ubyte_t *)object + Object_ChunkSize(object) &&
        object->rtti != NULL) {
#ifdef DEBUG_PRINT
        printf("large inner pointer: %p, object: %p\n", word, objectHeader);
        fflush(stdout);
#endif
        return object;
    } else {

        return NULL;
    }
}

Object *Object_GetLargeObject(LargeAllocator *allocator, void *word) {
    if (((uintptr_t)word & LARGE_BLOCK_MASK) != (uintptr_t)word) {
        word = (uintptr_t *)((uintptr_t)word & LARGE_BLOCK_MASK);
    }
    if (Bitmap_GetBit(allocator->bitmap, (ubyte_t *)word) &&
        Object_IsAllocated(&((Object *)word)->header)) {
        return (Object *)word;
    } else {
        Object *object = Object_getLargeInnerPointer(allocator, word);
        assert(object == NULL ||
               (word >= (uintptr_t *)object &&
                word < (uintptr_t *)Object_NextLargeObject(object)));
        return object;
    }
}

void Object_Mark(Object *object) {
    // Mark the object itself
    Object_MarkObjectHeader(&object->header);

    if (!Object_IsLargeObject(&object->header)) {
        // Mark the block
        BlockHeader *blockHeader = Block_GetBlockHeader(object);
        Block_Mark(blockHeader);

        // Mark all Lines
        int startIndex =
            Block_GetLineIndexFromWord(blockHeader, (uintptr_t *)object);
        uintptr_t *lastWord = (uintptr_t *)Object_NextObject(object) - 1;
        int endIndex = Block_GetLineIndexFromWord(blockHeader, lastWord);
        assert(startIndex >= 0 && startIndex < LINE_COUNT);
        assert(endIndex >= 0 && endIndex < LINE_COUNT);
        assert(startIndex <= endIndex);
        for (int i = startIndex; i <= endIndex; i++) {
            LineHeader *lineHeader = Block_GetLineHeader(blockHeader, i);
            Line_Mark(lineHeader);
        }
    }
}

size_t Object_ChunkSize(Object *object) {
    return MathUtils_RoundToNextMultiple(Object_Size(&object->header),
                                         MIN_BLOCK_SIZE);
}
