#ifndef IMMIX_GLOBAL_LARGE_ALLOCATOR_H
#define IMMIX_GLOBAL_LARGE_ALLOCATOR_H

#include "../GCTypes.h"
#include "../LargeAllocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A thin lock around the *existing*, unchanged `LargeAllocator`. None of
 * LargeAllocator.c's buddy-style free-list/sweep logic is touched - this
 * file only adds a mutex around the calls into it, the same philosophy as
 * GlobalBlockAllocator.
 */
typedef struct GlobalLargeAllocator GlobalLargeAllocator;

GlobalLargeAllocator *GlobalLargeAllocator_Create(void *offset, size_t size);

Object *GlobalLargeAllocator_GetBlock(GlobalLargeAllocator *allocator,
                                      size_t requestedBlockSize);

void GlobalLargeAllocator_Sweep(GlobalLargeAllocator *allocator);

/** Used by Heap_GrowLarge - grows the underlying size, its Bitmap, and
 *  registers the new address range as free chunks, all under one lock. */
void GlobalLargeAllocator_Grow(GlobalLargeAllocator *allocator,
                               void *chunkStart, size_t incrementBytes);

/**
 * Read-only access to the underlying `LargeAllocator*`, for
 * Marker_markConservative's call into Object_GetLargeObject. Safe without
 * taking the lock because it's only ever called by the active collector
 * thread during a stop-the-world pause, when no other thread can be
 * concurrently allocating from it.
 */
LargeAllocator *GlobalLargeAllocator_Underlying(GlobalLargeAllocator *allocator);

#ifdef __cplusplus
}
#endif

#endif // IMMIX_GLOBAL_LARGE_ALLOCATOR_H