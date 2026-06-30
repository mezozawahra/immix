# `concurrent/GlobalBlockAllocator.h` / `GlobalBlockAllocator.cpp`

The shared, locked pool of small-heap blocks every mutator's
[`ThreadLocalAllocator`](../ThreadLocalAllocator.md) pulls from. This file
is a deliberate "add a lock, don't reinvent the algorithm" wrapper: the
free-list logic underneath is the *exact same* unmodified
[`BlockList`](../datastructures/BlockList.md) the single-mutator codebase
always used — this file only adds synchronization around calls into it.

## The struct (C++-only, opaque to every C caller)

```c
struct GlobalBlockAllocator {
    std::mutex mutex;

    word_t *heapStart;
    uint64_t blockCount;

    BlockList freeBlocks;
    uint64_t freeBlockCount;
    BlockList recycledBlocks;
    uint64_t recycledBlockCount;

    size_t freeMemoryAfterCollection;
};
```

This is exactly the set of fields that used to live directly on the
single-mutator `Allocator` struct (now
[`ThreadLocalAllocator`](../ThreadLocalAllocator.md), which no longer has
any of these — see that doc's note on the split). The struct definition
itself only exists in the `.cpp` file; every other file in the codebase,
including C files like [`Block.c`](../Block.md) and
[`ThreadLocalAllocator.c`](../ThreadLocalAllocator.md), only ever sees the
opaque `typedef struct GlobalBlockAllocator GlobalBlockAllocator;` from
the header and talks to it exclusively through the `extern "C"` functions
below — this is what lets a `.c` file call into C++-implemented locking
without itself needing to become C++.

## Creation

```c
GlobalBlockAllocator *GlobalBlockAllocator_Create(word_t *heapStart, uint64_t blockCount) {
    auto *allocator = new GlobalBlockAllocator();
    ...
    BlockList_Init(&allocator->freeBlocks, heapStart);
    BlockList_Init(&allocator->recycledBlocks, heapStart);

    allocator->freeBlocks.first = (BlockHeader *)heapStart;
    BlockHeader *lastBlockHeader =
        (BlockHeader *)(heapStart + ((blockCount - 1) * WORDS_IN_BLOCK));
    allocator->freeBlocks.last = lastBlockHeader;
    lastBlockHeader->header.nextBlock = LAST_BLOCK;

    allocator->freeBlockCount = blockCount;
    ...
}
```

Seeds the entire initial small heap as one giant free run — identical
logic to what `Allocator_Create` used to do directly, just relocated here
since [`Heap_Create`](../Heap.md) now builds this instead of building a
single `Allocator` (see [Heap.md](../Heap.md) for the full picture of what
moved where).

## The allocation-facing API

```c
BlockHeader *GlobalBlockAllocator_GetBlock(GlobalBlockAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    if (!BlockList_IsEmpty(&allocator->recycledBlocks)) {
        return BlockList_RemoveFirstBlock(&allocator->recycledBlocks);
    }
    if (!BlockList_IsEmpty(&allocator->freeBlocks)) {
        return BlockList_RemoveFirstBlock(&allocator->freeBlocks);
    }
    return NULL;
}
```

Called from `ThreadLocalAllocator_getNextBlock` — recycled blocks first,
then free blocks, same policy as before (use up partially-full blocks
before opening a virgin one, for density/locality), just now behind a
`std::lock_guard`. Every call is an independent critical section: a thread
takes the lock, removes at most one block, releases it. There's no
batching of multiple block requests into one lock acquisition — each
`ThreadLocalAllocator_getNextLine`/`getNextBlock` call that needs a new
block pays one lock/unlock.

```c
BlockHeader *GlobalBlockAllocator_GetFreeBlock(GlobalBlockAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    if (BlockList_IsEmpty(&allocator->freeBlocks)) {
        return NULL;
    }
    return BlockList_RemoveFirstBlock(&allocator->freeBlocks);
}
```

A separate entry point that **only** ever looks at `freeBlocks`, never
`recycledBlocks`. Used for overflow allocation
(`ThreadLocalAllocator_overflowAllocation`) and for priming the overflow
cursor in `ThreadLocalAllocator_InitCursors` — preserving exactly the
reasoning from the single-mutator design: a free block is guaranteed
contiguous from byte zero (no hole-hunting needed for a multi-line
object), and keeping overflow allocation off `recycledBlocks` means it can
never end up fighting the normal cursor over the same holes in the same
block. Multithreading didn't change this reasoning at all — it just now
also means two different *threads'* overflow cursors can't collide with
each other's normal cursors either, for the same underlying reason.

```c
void GlobalBlockAllocator_AddFreeBlocks(GlobalBlockAllocator *allocator,
                                        BlockHeader *first, BlockHeader *last,
                                        uint64_t count) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    BlockList_AddBlocksLast(&allocator->freeBlocks, first, last);
    allocator->blockCount += count;
    allocator->freeBlockCount += count;
}
```

Used by `Heap_Grow` to bulk-append an already block-chained range of
brand-new memory in one O(1) operation (`BlockList_AddBlocksLast` doesn't
walk the chain — it just splices `first..last` onto the end of the
existing list, relying on the new range already being internally linked).

## The recycle-phase API

```c
void GlobalBlockAllocator_BeginRecycle(GlobalBlockAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    BlockList_Clear(&allocator->freeBlocks);
    BlockList_Clear(&allocator->recycledBlocks);
    allocator->freeBlockCount = 0;
    allocator->recycledBlockCount = 0;
    allocator->freeMemoryAfterCollection = 0;
}
```

Called once, at the very start of [`Heap_Recycle`](../Heap.md) — the
direct replacement for what used to be a few inline
`BlockList_Clear`/counter-reset lines at the top of `Heap_Recycle` itself.
Wipes both lists and every counter back to zero, since `Heap_Recycle` is
about to rebuild them entirely from a fresh sweep over the whole small
heap.

```c
void GlobalBlockAllocator_AddFreeBlock(GlobalBlockAllocator *allocator, BlockHeader *block);
void GlobalBlockAllocator_AddRecycledBlock(GlobalBlockAllocator *allocator, BlockHeader *block);
void GlobalBlockAllocator_AddFreeMemory(GlobalBlockAllocator *allocator, size_t bytes);
```

Each is a one-block (or one-batch, for `AddFreeMemory`) locked append,
called from [`Block_Recycle`](../Block.md) as it sweeps. See
[Block.md](../Block.md) for exactly how `Block_Recycle` batches its
`AddFreeMemory` calls (once per block, not once per recycled line) to
avoid needless lock contention during the sweep.

These functions are documented as "only ever called by the active
collector thread, during a stop-the-world pause" and locked anyway —
defensively, in case sweeping is ever parallelized in the future, not
because concurrent callers are expected today. Worth knowing if you're
reasoning about lock contention: right now, every call into this section
happens from a single thread in sequence, so the lock here is pure
overhead until/unless that assumption changes.

## The growth-heuristic API

```c
bool GlobalBlockAllocator_CanInitCursors(GlobalBlockAllocator *allocator, uint32_t threadCount) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    uint64_t n = threadCount == 0 ? 1 : (uint64_t)threadCount;
    return allocator->freeBlockCount >= 2 * n ||
           (allocator->freeBlockCount >= n &&
            allocator->freeBlockCount + allocator->recycledBlockCount >= 2 * n);
}
```

The single-mutator version asked "are there enough blocks for *one*
allocator's two cursors" (one free block for overflow, one free-or-recycled
block for normal). This version asks the same question scaled by
`threadCount`: every registered thread will independently need to
re-prime both its own cursors after a collection, so the heap needs `n`
free blocks for everyone's overflow cursor, plus `n` more
free-or-recycled blocks for everyone's normal cursor. The `threadCount == 0`
guard treats zero registered threads as `n = 1`, avoiding a degenerate
"trivially satisfied" result before any thread has registered yet.

```c
bool GlobalBlockAllocator_ShouldGrow(GlobalBlockAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    uint64_t unavailableBlockCount =
        allocator->blockCount - (allocator->freeBlockCount + allocator->recycledBlockCount);

    return allocator->freeBlockCount < allocator->blockCount / 3 ||
           4 * unavailableBlockCount > allocator->blockCount ||
           allocator->freeMemoryAfterCollection * 2 < allocator->blockCount * BLOCK_TOTAL_SIZE;
}
```

The exact same three-part heuristic from the single-mutator
`Allocator_ShouldGrow` (fewer than a third of blocks free; more than a
quarter of blocks fully unavailable; last collection reclaimed less than
half the heap), unchanged in substance — only relocated here and locked,
since `blockCount`/`freeBlockCount`/`recycledBlockCount`/
`freeMemoryAfterCollection` all live here now instead of on a
single-owner `Allocator`. The original's `#ifdef DEBUG_PRINT` diagnostic
block (a handful of `printf`s of these same counters) was dropped during
the move rather than carried over — easy to re-add here if you still want
that diagnostic, since this is now the one place all four counters live.