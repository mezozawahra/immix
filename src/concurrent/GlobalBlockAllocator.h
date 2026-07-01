#ifndef IMMIX_GLOBAL_BLOCK_ALLOCATOR_H
#define IMMIX_GLOBAL_BLOCK_ALLOCATOR_H

#include "../GCTypes.h"
#include "../headers/BlockHeader.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Everything that used to be `freeBlocks`/`recycledBlocks`/their counts on
 * the single-mutator `Allocator` struct, moved here and locked, since it's
 * now shared by every mutator thread's own `Allocator`. The struct itself
 * is only defined in GlobalBlockAllocator.cpp (it holds a std::mutex) -
 * everywhere else it's just an opaque handle.
 */
typedef struct GlobalBlockAllocator GlobalBlockAllocator;

GlobalBlockAllocator *GlobalBlockAllocator_Create(uintptr_t *heapStart,
                                                  uint64_t blockCount);

/** Recycled blocks first, then free blocks. NULL if both are empty. */
BlockHeader *GlobalBlockAllocator_GetBlock(GlobalBlockAllocator *allocator);

/**
 * Free blocks only - never recycled. Used by overflow allocation, and by
 * Allocator_InitCursors for the overflow cursor, for exactly the reason
 * overflow allocation never touched recycled blocks in the original
 * single-mutator code: a free block is guaranteed contiguous, and it keeps
 * overflow allocation from fighting the normal cursor over the same holes.
 */
BlockHeader *GlobalBlockAllocator_GetFreeBlock(GlobalBlockAllocator *allocator);

/** Bulk-append an already-chained run of brand-new blocks (Heap_Grow). */
void GlobalBlockAllocator_AddFreeBlocks(GlobalBlockAllocator *allocator,
                                        BlockHeader *first, BlockHeader *last,
                                        uint64_t count);

/**
 * The recycle-phase API. Only ever called by the active collector thread,
 * during a stop-the-world pause, from Block_Recycle (Block.c). Still
 * locked, defensively, in case sweeping is ever parallelized later.
 */
void GlobalBlockAllocator_BeginRecycle(GlobalBlockAllocator *allocator);
void GlobalBlockAllocator_AddFreeBlock(GlobalBlockAllocator *allocator,
                                       BlockHeader *block);
void GlobalBlockAllocator_AddRecycledBlock(GlobalBlockAllocator *allocator,
                                           BlockHeader *block);
void GlobalBlockAllocator_AddFreeMemory(GlobalBlockAllocator *allocator,
                                        size_t bytes);

/**
 * `threadCount` is how many mutator threads are currently registered -
 * every one of them will need to re-prime its own normal *and* overflow
 * cursor after this recycle, so the heap needs enough spare blocks for
 * everyone, not just one Allocator the way the original heuristic assumed.
 */
bool GlobalBlockAllocator_CanInitCursors(GlobalBlockAllocator *allocator,
                                         uint32_t threadCount);
bool GlobalBlockAllocator_ShouldGrow(GlobalBlockAllocator *allocator);

#ifdef __cplusplus
}
#endif

#endif // IMMIX_GLOBAL_BLOCK_ALLOCATOR_H