#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include "Heap.h"
#include "Block.h"
#include "Log.h"
#include "Marker.h"
#include "State.h"
#include "utils/MathUtils.h"
#include "StackTrace.h"
#include "Memory.h"
#include "concurrent/MutatorSync.h"

#define HEAP_MEM_PROT (PROT_READ | PROT_WRITE)
#define HEAP_MEM_FLAGS (MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS)
#define HEAP_MEM_FD -1
#define HEAP_MEM_FD_OFFSET 0

size_t Heap_getMemoryLimit() { return getMemorySize(); }

word_t *Heap_mapAndAlign(size_t memoryLimit, size_t alignmentSize) {
    word_t *heapStart = mmap(NULL, memoryLimit, HEAP_MEM_PROT, HEAP_MEM_FLAGS,
                             HEAP_MEM_FD, HEAP_MEM_FD_OFFSET);

    size_t alignmentMask = ~(alignmentSize - 1);
    if (((word_t)heapStart & alignmentMask) != (word_t)heapStart) {
        word_t *previousBlock =
            (word_t *)((word_t)heapStart & BLOCK_SIZE_IN_BYTES_INVERSE_MASK);
        heapStart = previousBlock + WORDS_IN_BLOCK;
    }
    return heapStart;
}

Heap *Heap_Create(size_t initialSize) {
    assert(initialSize >= 2 * BLOCK_TOTAL_SIZE);
    assert(initialSize % BLOCK_TOTAL_SIZE == 0);

    Heap *heap = malloc(sizeof(Heap));

    size_t memoryLimit = Heap_getMemoryLimit();
    heap->memoryLimit = memoryLimit;

    word_t *smallHeapStart = Heap_mapAndAlign(memoryLimit, BLOCK_TOTAL_SIZE);

    heap->smallHeapSize = initialSize;
    heap->heapStart = smallHeapStart;
    heap->heapEnd = smallHeapStart + initialSize / WORD_SIZE;
    heap->globalBlockAllocator = GlobalBlockAllocator_Create(
        smallHeapStart, initialSize / BLOCK_TOTAL_SIZE);

    word_t *largeHeapStart = Heap_mapAndAlign(memoryLimit, MIN_BLOCK_SIZE);
    heap->largeHeapSize = initialSize;
    heap->globalLargeAllocator =
        GlobalLargeAllocator_Create(largeHeapStart, initialSize);
    heap->largeHeapStart = largeHeapStart;
    heap->largeHeapEnd = (word_t *)((ubyte_t *)largeHeapStart + initialSize);

    return heap;
}

/**
 * Allocates large objects via the shared GlobalLargeAllocator. Note this
 * also fixes a latent bug in the original: the original's second retry
 * branch (after the first collect) forgot to call Object_SetObjectArray -
 * every path here sets it consistently.
 */
word_t *Heap_AllocLarge(Heap *heap, uint32_t objectSize, bool isObjectArray,
                        MutatorThread *self) {
    uint32_t size = objectSize + OBJECT_HEADER_SIZE;
    assert(objectSize % WORD_SIZE == 0);
    assert(size >= MIN_BLOCK_SIZE);

    Object *object =
        GlobalLargeAllocator_GetBlock(heap->globalLargeAllocator, size);

    if (object == NULL) {
        Heap_Collect(heap, self);
        object =
            GlobalLargeAllocator_GetBlock(heap->globalLargeAllocator, size);
        if (object == NULL) {
            Heap_GrowLarge(heap, size);
            object = GlobalLargeAllocator_GetBlock(
                heap->globalLargeAllocator, size);
            assert(object != NULL);
        }
    }

    ObjectHeader *objectHeader = &object->header;
    Object_SetObjectType(objectHeader, object_large);
    Object_SetObjectArray(objectHeader, isObjectArray);
    Object_SetSize(objectHeader, size);
    return Object_ToMutatorAddress(object);
}

word_t *Heap_allocSmallSlow(Heap *heap, uint32_t size, bool isObjectArray,
                            MutatorThread *self) {
    Heap_Collect(heap, self);

    Object *object = (Object *)ThreadLocalAllocator_Alloc(self->allocator, size);
    if (object == NULL) {
        Heap_Grow(heap, size);
        object = (Object *)ThreadLocalAllocator_Alloc(self->allocator, size);
        assert(object != NULL);
    }

    ObjectHeader *objectHeader = &object->header;
    Object_SetObjectType(objectHeader, object_standard);
    Object_SetObjectArray(objectHeader, isObjectArray);
    Object_SetSize(objectHeader, size);
    Object_SetAllocated(objectHeader);

    return Object_ToMutatorAddress(object);
}

INLINE word_t *Heap_AllocSmall(Heap *heap, uint32_t objectSize,
                               bool isObjectArray, MutatorThread *self) {
    uint32_t size = objectSize + OBJECT_HEADER_SIZE;
    assert(objectSize % WORD_SIZE == 0);
    assert(size < MIN_BLOCK_SIZE);

    Object *object = (Object *)ThreadLocalAllocator_Alloc(self->allocator, size);
    if (object != NULL) {
        ObjectHeader *objectHeader = &object->header;
        Object_SetObjectType(objectHeader, object_standard);
        Object_SetObjectArray(objectHeader, isObjectArray);
        Object_SetSize(objectHeader, size);
        Object_SetAllocated(objectHeader);

        return Object_ToMutatorAddress(object);
    } else {
        return Heap_allocSmallSlow(heap, size, isObjectArray, self);
    }
}

word_t *Heap_Alloc(Heap *heap, uint32_t objectSize, bool isObjectArray,
                   MutatorThread *self) {
    assert(objectSize % WORD_SIZE == 0);

    if (objectSize + OBJECT_HEADER_SIZE >= LARGE_BLOCK_SIZE) {
        return Heap_AllocLarge(heap, objectSize, isObjectArray, self);
    } else {
        return Heap_AllocSmall(heap, objectSize, isObjectArray, self);
    }
}

/**
 * `self` is the thread that ran out of memory and wants a collection.
 * MutatorSync_BeginCollection decides who actually drives it: if `self`
 * loses the race, it was parked for the pause's duration and this just
 * returns - the caller's retry-the-allocation-now logic (in
 * Heap_AllocLarge / Heap_allocSmallSlow) needs no special case either way.
 */
void Heap_Collect(Heap *heap, MutatorThread *self) {
    if (!MutatorSync_BeginCollection(self)) {
        return;
    }

    Marker_MarkRoots(heap, stack);
    Heap_Recycle(heap);

    MutatorSync_EndCollection();
}

static void Heap_reinitThreadCursors(MutatorThread *thread, void *userData) {
    ThreadLocalAllocator_InitCursors(thread->allocator);
}

/**
 * Only ever called while every mutator is parked (from Heap_Collect, by
 * the thread that won MutatorSync_BeginCollection), so the block-by-block
 * sweep and the global allocator's bookkeeping need no per-call locking
 * beyond what GlobalBlockAllocator/GlobalLargeAllocator already do
 * defensively.
 */
void Heap_Recycle(Heap *heap) {
    GlobalBlockAllocator_BeginRecycle(heap->globalBlockAllocator);

    word_t *current = heap->heapStart;
    while (current != heap->heapEnd) {
        BlockHeader *blockHeader = (BlockHeader *)current;
        Block_Recycle(heap->globalBlockAllocator, blockHeader);
        current += WORDS_IN_BLOCK;
    }
    GlobalLargeAllocator_Sweep(heap->globalLargeAllocator);

    uint32_t threadCount = MutatorSync_ThreadCount();
    if (!GlobalBlockAllocator_CanInitCursors(heap->globalBlockAllocator,
                                             threadCount) ||
        GlobalBlockAllocator_ShouldGrow(heap->globalBlockAllocator)) {
        size_t increment = heap->smallHeapSize / WORD_SIZE * GROWTH_RATE / 100;
        increment =
            (increment - 1 + WORDS_IN_BLOCK) / WORDS_IN_BLOCK * WORDS_IN_BLOCK;
        Heap_Grow(heap, increment);
    }

    // Every registered thread (not just the one that triggered this
    // collection) needs its cursors re-primed before it can allocate
    // again.
    MutatorSync_ForEachThread(Heap_reinitThreadCursors, NULL);
}

void Heap_exitWithOutOfMemory() {
    printf("Out of heap space\n");
    StackTrace_PrintStackTrace();
    exit(1);
}

bool Heap_isGrowingPossible(Heap *heap, size_t increment) {
    return heap->smallHeapSize + heap->largeHeapSize + increment * WORD_SIZE <=
           heap->memoryLimit;
}

/** Grows the small heap by at least `increment` words */
void Heap_Grow(Heap *heap, size_t increment) {
    assert(increment % WORDS_IN_BLOCK == 0);

    if (!Heap_isGrowingPossible(heap, increment)) {
        if (GlobalBlockAllocator_CanInitCursors(heap->globalBlockAllocator,
                                                MutatorSync_ThreadCount())) {
            return;
        } else {
            Heap_exitWithOutOfMemory();
        }
    }

    word_t *heapEnd = heap->heapEnd;
    heap->heapEnd = heapEnd + increment;
    heap->smallHeapSize += increment * WORD_SIZE;

    BlockHeader *lastBlock = (BlockHeader *)(heap->heapEnd - WORDS_IN_BLOCK);
    GlobalBlockAllocator_AddFreeBlocks(heap->globalBlockAllocator,
                                       (BlockHeader *)heapEnd, lastBlock,
                                       increment / WORDS_IN_BLOCK);
}

/** Grows the large heap by at least `increment` words */
void Heap_GrowLarge(Heap *heap, size_t increment) {
    increment = 1UL << MathUtils_Log2Ceil(increment);

    if (heap->smallHeapSize + heap->largeHeapSize + increment * WORD_SIZE >
        heap->memoryLimit) {
        Heap_exitWithOutOfMemory();
    }

    word_t *heapEnd = heap->largeHeapEnd;
    heap->largeHeapEnd += increment;
    heap->largeHeapSize += increment * WORD_SIZE;

    GlobalLargeAllocator_Grow(heap->globalLargeAllocator, heapEnd,
                              increment * WORD_SIZE);
}