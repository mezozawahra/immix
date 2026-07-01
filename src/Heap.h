#ifndef IMMIX_HEAP_H
#define IMMIX_HEAP_H

#include "GCTypes.h"
#include "ThreadLocalAllocator.h"
#include "LargeAllocator.h"
#include "datastructures/Stack.h"
#include "concurrent/GlobalBlockAllocator.h"
#include "concurrent/GlobalLargeAllocator.h"
#include "concurrent/MutatorThread.h"

typedef struct {
    size_t memoryLimit;
    uintptr_t *heapStart;
    uintptr_t *heapEnd;
    size_t smallHeapSize;
    uintptr_t *largeHeapStart;
    uintptr_t *largeHeapEnd;
    size_t largeHeapSize;

    GlobalBlockAllocator *globalBlockAllocator;
    GlobalLargeAllocator *globalLargeAllocator;
} Heap;

static inline bool Heap_IsWordInLargeHeap(Heap *heap, uintptr_t *word) {
    return word != NULL && word >= heap->largeHeapStart &&
           word < heap->largeHeapEnd;
}

static inline bool Heap_IsWordInSmallHeap(Heap *heap, uintptr_t *word) {
    return word != NULL && word >= heap->heapStart && word < heap->heapEnd;
}

static inline bool Heap_IsWordInHeap(Heap *heap, uintptr_t *word) {
    return Heap_IsWordInSmallHeap(heap, word) ||
           Heap_IsWordInLargeHeap(heap, word);
}
static inline bool heap_isObjectInHeap(Heap *heap, Object *object) {
    return Heap_IsWordInHeap(heap, (uintptr_t *)object);
}

Heap *Heap_Create(size_t initialHeapSize);

// Every allocation entry point now needs the calling thread's
// MutatorThread, both for its ThreadLocalAllocator and so Heap_Collect can
// drive (or park for) a collection through MutatorSync.
uintptr_t *Heap_Alloc(Heap *heap, uint32_t objectSize, bool isObjectArray,
                   MutatorThread *self);
uintptr_t *Heap_AllocSmall(Heap *heap, uint32_t objectSize, bool isObjectArray,
                        MutatorThread *self);
uintptr_t *Heap_AllocLarge(Heap *heap, uint32_t objectSize, bool isObjectArray,
                        MutatorThread *self);

void Heap_Collect(Heap *heap, MutatorThread *self);

void Heap_Recycle(Heap *heap);
void Heap_Grow(Heap *heap, size_t increment);
void Heap_GrowLarge(Heap *heap, size_t increment);

#endif // IMMIX_HEAP_H