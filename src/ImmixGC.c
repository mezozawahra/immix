#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include "GCTypes.h"
#include "Heap.h"
#include "ImmixGC.h"
#include "datastructures/Stack.h"
#include "Marker.h"
#include "Log.h"
#include "Object.h"
#include "State.h"
#include "utils/MathUtils.h"
#include "concurrent/MutatorSync.h"

#define INITIAL_HEAP_SIZE (1024 * 1024UL)

void scalanative_init() {
    heap = Heap_Create(INITIAL_HEAP_SIZE);
    stack = Stack_Alloc(INITIAL_STACK_SIZE);
}

void ImmixGC_RegisterThread(Val *stackBottom) {
    MutatorThread *thread = MutatorSync_RegisterThread(stackBottom);
    thread->allocator =
        ThreadLocalAllocator_Create(heap->globalBlockAllocator);
    currentMutatorThread = thread;
}

void ImmixGC_UnregisterThread() {
    MutatorSync_UnregisterThread(currentMutatorThread);
    currentMutatorThread = NULL;
}

void *scalanative_alloc(void *info, size_t size, int isObjectArray) {
    MutatorSync_Poll(currentMutatorThread);
    size = MathUtils_RoundToNextMultiple(size, WORD_SIZE);

    void **alloc = (void **)Heap_Alloc(heap, size, isObjectArray ? true : false,
                                       currentMutatorThread);
    *alloc = info;
    return (void *)alloc;
}

void *scalanative_alloc_small(void *info, size_t size) {
    MutatorSync_Poll(currentMutatorThread);
    size = MathUtils_RoundToNextMultiple(size, WORD_SIZE);

    void **alloc =
        (void **)Heap_AllocSmall(heap, size, false, currentMutatorThread);
    *alloc = info;
    return (void *)alloc;
}

void *scalanative_alloc_large(void *info, size_t size) {
    MutatorSync_Poll(currentMutatorThread);
    size = MathUtils_RoundToNextMultiple(size, WORD_SIZE);

    void **alloc =
        (void **)Heap_AllocLarge(heap, size, false, currentMutatorThread);
    *alloc = info;
    return (void *)alloc;
}

void *scalanative_alloc_atomic(void *info, size_t size) {
    return scalanative_alloc(info, size, false);
}

void scalanative_collect() { Heap_Collect(heap, currentMutatorThread); }