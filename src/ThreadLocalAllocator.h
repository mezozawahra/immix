#ifndef IMMIX_THREAD_LOCAL_ALLOCATOR_H
#define IMMIX_THREAD_LOCAL_ALLOCATOR_H

#include "GCTypes.h"
#include <stddef.h>
#include "concurrent/GlobalBlockAllocator.h"

typedef struct {
    // The shared pool this thread's allocator pulls blocks from. Never
    // touched directly here - only through the GlobalBlockAllocator_*
    // functions, which take care of locking.
    GlobalBlockAllocator *globalAllocator;

    BlockHeader *block;
    uintptr_t *cursor;
    uintptr_t *limit;

    BlockHeader *largeBlock;
    uintptr_t *largeCursor;
    uintptr_t *largeLimit;
} ThreadLocalAllocator;

ThreadLocalAllocator *ThreadLocalAllocator_Create(GlobalBlockAllocator *globalAllocator);
bool ThreadLocalAllocator_CanInitCursors(ThreadLocalAllocator *allocator);
void ThreadLocalAllocator_InitCursors(ThreadLocalAllocator *allocator);
uintptr_t *ThreadLocalAllocator_Alloc(ThreadLocalAllocator *allocator, size_t size);

bool ThreadLocalAllocator_ShouldGrow(ThreadLocalAllocator *allocator);

#endif // IMMIX_THREAD_LOCAL_ALLOCATOR_H