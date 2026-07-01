#include <mutex>
#include "GlobalBlockAllocator.h"
#include "../datastructures/BlockList.h"
#include "../Constants.h"

/**
 * The real definition, invisible to C - this is exactly the set of fields
 * that used to live directly on `Allocator`. Note it deliberately reuses
 * the existing C `BlockList`/`BlockList_*` logic unchanged: this file adds
 * synchronization, it does not reimplement the free-list algorithm.
 */
struct GlobalBlockAllocator {
    std::mutex mutex;

    uintptr_t *heapStart;
    uint64_t blockCount;

    BlockList freeBlocks;
    uint64_t freeBlockCount;
    BlockList recycledBlocks;
    uint64_t recycledBlockCount;

    size_t freeMemoryAfterCollection;
};

extern "C" GlobalBlockAllocator *GlobalBlockAllocator_Create(uintptr_t *heapStart,
                                                              uint64_t blockCount) {
    auto *allocator = new GlobalBlockAllocator();
    allocator->heapStart = heapStart;
    allocator->blockCount = blockCount;

    BlockList_Init(&allocator->freeBlocks, heapStart);
    BlockList_Init(&allocator->recycledBlocks, heapStart);

    // Seed the entire initial heap as one free run - same as
    // Allocator_Create used to do directly.
    allocator->freeBlocks.first = (BlockHeader *)heapStart;
    BlockHeader *lastBlockHeader =
        (BlockHeader *)(heapStart + ((blockCount - 1) * SLOTS_IN_BLOCK));
    allocator->freeBlocks.last = lastBlockHeader;
    lastBlockHeader->header.nextBlock = LAST_BLOCK;

    allocator->freeBlockCount = blockCount;
    allocator->recycledBlockCount = 0;
    allocator->freeMemoryAfterCollection = 0;

    return allocator;
}

extern "C" BlockHeader *GlobalBlockAllocator_GetBlock(GlobalBlockAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    if (!BlockList_IsEmpty(&allocator->recycledBlocks)) {
        return BlockList_RemoveFirstBlock(&allocator->recycledBlocks);
    }
    if (!BlockList_IsEmpty(&allocator->freeBlocks)) {
        return BlockList_RemoveFirstBlock(&allocator->freeBlocks);
    }
    return NULL;
}

extern "C" BlockHeader *GlobalBlockAllocator_GetFreeBlock(GlobalBlockAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    if (BlockList_IsEmpty(&allocator->freeBlocks)) {
        return NULL;
    }
    return BlockList_RemoveFirstBlock(&allocator->freeBlocks);
}

extern "C" void GlobalBlockAllocator_AddFreeBlocks(GlobalBlockAllocator *allocator,
                                                    BlockHeader *first,
                                                    BlockHeader *last,
                                                    uint64_t count) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    BlockList_AddBlocksLast(&allocator->freeBlocks, first, last);
    allocator->blockCount += count;
    allocator->freeBlockCount += count;
}

extern "C" void GlobalBlockAllocator_BeginRecycle(GlobalBlockAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    BlockList_Clear(&allocator->freeBlocks);
    BlockList_Clear(&allocator->recycledBlocks);
    allocator->freeBlockCount = 0;
    allocator->recycledBlockCount = 0;
    allocator->freeMemoryAfterCollection = 0;
}

extern "C" void GlobalBlockAllocator_AddFreeBlock(GlobalBlockAllocator *allocator,
                                                   BlockHeader *block) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    BlockList_AddLast(&allocator->freeBlocks, block);
    allocator->freeBlockCount++;
}

extern "C" void GlobalBlockAllocator_AddRecycledBlock(GlobalBlockAllocator *allocator,
                                                       BlockHeader *block) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    BlockList_AddLast(&allocator->recycledBlocks, block);
    allocator->recycledBlockCount++;
}

extern "C" void GlobalBlockAllocator_AddFreeMemory(GlobalBlockAllocator *allocator,
                                                    size_t bytes) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    allocator->freeMemoryAfterCollection += bytes;
}

extern "C" bool GlobalBlockAllocator_CanInitCursors(GlobalBlockAllocator *allocator,
                                                     uint32_t threadCount) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    uint64_t n = threadCount == 0 ? 1 : (uint64_t)threadCount;
    // Each thread needs one free block (overflow cursor) and one
    // free-or-recycled block (normal cursor) to re-prime itself.
    return allocator->freeBlockCount >= 2 * n ||
           (allocator->freeBlockCount >= n &&
            allocator->freeBlockCount + allocator->recycledBlockCount >= 2 * n);
}

extern "C" bool GlobalBlockAllocator_ShouldGrow(GlobalBlockAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    uint64_t unavailableBlockCount =
        allocator->blockCount -
        (allocator->freeBlockCount + allocator->recycledBlockCount);

    return allocator->freeBlockCount < allocator->blockCount / 3 ||
           4 * unavailableBlockCount > allocator->blockCount ||
           allocator->freeMemoryAfterCollection * 2 <
               allocator->blockCount * BLOCK_TOTAL_SIZE;
}