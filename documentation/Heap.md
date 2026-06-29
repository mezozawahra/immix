# `Heap.h` / `Heap.c`

The top-level object every other subsystem hangs off of. It owns one
`Allocator` (the small/normal heap) and one `LargeAllocator` (the large
heap, ≥8KB objects) as two **physically separate** mmap'd regions — not
one heap split by a size threshold at some address, but two independent
address ranges each grown independently. Every `Heap_IsWordInSmallHeap`/
`Heap_IsWordInLargeHeap` check elsewhere in the codebase is a simple range
comparison against these two regions.

## The struct

```c
typedef struct {
    size_t memoryLimit;
    word_t *heapStart;       word_t *heapEnd;       size_t smallHeapSize;
    word_t *largeHeapStart;  word_t *largeHeapEnd;   size_t largeHeapSize;
    Allocator *allocator;
    LargeAllocator *largeAllocator;
} Heap;
```

`memoryLimit` is read once at creation via `getMemorySize()`
([Memory.md](Memory.md)) — total physical RAM — and acts as a hard ceiling
neither `heapEnd` nor `largeHeapEnd` will ever be grown past
(`Heap_isGrowingPossible` checks `smallHeapSize + largeHeapSize +
increment <= memoryLimit`; if growth would cross it, the process aborts
via `Heap_exitWithOutOfMemory`, which prints a stack trace via
[StackTrace.h](StackTrace.md) and calls `exit(1)`).

## Mapping memory: `Heap_mapAndAlign`

```c
word_t *Heap_mapAndAlign(size_t memoryLimit, size_t alignmentSize) {
    word_t *heapStart = mmap(NULL, memoryLimit, HEAP_MEM_PROT, HEAP_MEM_FLAGS, -1, 0);
    size_t alignmentMask = ~(alignmentSize - 1);
    if (((word_t)heapStart & alignmentMask) != (word_t)heapStart) {
        word_t *previousBlock = (word_t *)((word_t)heapStart & BLOCK_SIZE_IN_BYTES_INVERSE_MASK);
        heapStart = previousBlock + WORDS_IN_BLOCK;
    }
    return heapStart;
}
```

Two things worth calling out:

- **The whole `memoryLimit` (i.e. all of physical RAM) gets mmap'd up
  front**, with `MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS`. This is the
  "reserve a huge chunk of address space cheaply, let the OS commit actual
  pages lazily as they're touched" pattern — `MAP_NORESERVE` specifically
  tells the kernel not to pre-reserve swap/overcommit accounting for the
  whole range, since most of it will never actually be written. This is
  why `Heap_Grow` later can just move `heapEnd` further into this
  already-mapped region without a second `mmap` call.
- **Manual realignment**: `mmap` only guarantees page alignment, not
  alignment to `BLOCK_TOTAL_SIZE` (32KB, called with `alignmentSize` for
  the small heap) or `MIN_BLOCK_SIZE` (8KB, for the large heap). If the
  address mmap actually returned isn't already aligned, this rounds
  *forward* to the next aligned block boundary (sacrificing the few KB
  before that boundary — acceptable, since the requested mapping was the
  size of all of RAM, there's slack to spare). This alignment guarantee is
  what every pointer-masking trick elsewhere in the codebase
  (`Block_GetBlockHeader`, the large heap's `LARGE_BLOCK_MASK` rounding)
  depends on.

## `Heap_Alloc` — the size-based fork

```c
word_t *Heap_Alloc(Heap *heap, uint32_t objectSize, bool isObjectArray) {
    if (objectSize + OBJECT_HEADER_SIZE >= LARGE_BLOCK_SIZE) {
        return Heap_AllocLarge(heap, objectSize, isObjectArray);
    } else {
        return Heap_AllocSmall(heap, objectSize, isObjectArray);
    }
}
```

This is the *only* place that decides which of the two heaps an object
goes to, and it's purely a size comparison against `LARGE_BLOCK_SIZE`
(8KB) — nothing about the object's type matters. Note this check includes
the header (`objectSize + OBJECT_HEADER_SIZE`), so an object whose raw
field data is just under 8KB but pushes the *total* allocation over the
line still goes to the large heap.

## `Heap_AllocSmall` / `Heap_allocSmallSlow` — the retry ladder

```c
INLINE word_t *Heap_AllocSmall(Heap *heap, uint32_t objectSize, bool isObjectArray) {
    uint32_t size = objectSize + OBJECT_HEADER_SIZE;
    Object *object = (Object *)Allocator_Alloc(heap->allocator, size);
    if (object != NULL) {
        /* stamp type/size/array-flag/allocated, return mutator address */
    } else {
        return Heap_allocSmallSlow(heap, size, isObjectArray);
    }
}
```

Try the fast path once. On failure (`Allocator_Alloc` returned `NULL` —
out of lines and blocks, see [Allocator.md](Allocator.md)), fall to the
slow path:

```c
word_t *Heap_allocSmallSlow(Heap *heap, uint32_t size, bool isObjectArray) {
    Heap_Collect(heap, stack);
    Object *object = (Object *)Allocator_Alloc(heap->allocator, size);
    if (object == NULL) {
        Heap_Grow(heap, size);
        object = (Object *)Allocator_Alloc(heap->allocator, size);
        assert(object != NULL);   // if this fails, it's a real OOM with no further recovery
    }
    /* stamp metadata, return */
}
```

Collect, retry; if it *still* fails, grow the heap, retry once more — and
if that third attempt still fails, the code just `assert`s success rather
than handling the failure explicitly. In a build where `NDEBUG` is defined
(see [Log.md](Log.md) — which is the default in this codebase, since
`Log.h` defines `NDEBUG` before including `<assert.h>`), that assert
compiles away to nothing, and a failed third attempt would silently return
a garbage/uninitialized pointer instead of crashing loudly. Worth knowing
if you're chasing a heisenbug under real memory pressure — this path
currently has no real out-of-memory handling of its own (unlike the large
path below, or `Heap_Grow` itself, which do call
`Heap_exitWithOutOfMemory` when growth itself is impossible).

## `Heap_AllocLarge` — the same ladder, different sub-allocator

Mirrors the small path almost exactly: try `LargeAllocator_GetBlock`,
collect-and-retry on failure, `Heap_GrowLarge`-and-retry as a last resort.
The large path's structure was written before the small path's helper got
factored out into `Heap_allocSmallSlow`, so it's inlined directly in
`Heap_AllocLarge` rather than split into its own slow-path function — same
logic, just not extracted.

## `Heap_Collect` — the whole cycle in two calls

```c
void Heap_Collect(Heap *heap, Stack *stack) {
    Marker_MarkRoots(heap, stack);
    Heap_Recycle(heap);
}
```

That's the entire collection cycle: trace everything reachable
([Marker.md](Marker.md)), then rebuild the free/recycled lists from
whatever's left ([Block.md](Block.md), and the large-heap sweep below).
There's no explicit "reset all marks to zero" step before the *next*
collection's trace begins — `Block_recycleMarkedLine`/`Block_Recycle`
unmark exactly as they sweep (every line and block visited gets unmarked
on the way through), and `LargeAllocator_Sweep` overwrites every chunk's
flag outright. The heap always exits a collection already correctly
unmarked for the next one.

## `Heap_Recycle`

```c
void Heap_Recycle(Heap *heap) {
    BlockList_Clear(&heap->allocator->recycledBlocks);
    BlockList_Clear(&heap->allocator->freeBlocks);
    heap->allocator->freeBlockCount = 0;
    heap->allocator->recycledBlockCount = 0;
    heap->allocator->freeMemoryAfterCollection = 0;

    word_t *current = heap->heapStart;
    while (current != heap->heapEnd) {
        Block_Recycle(heap->allocator, (BlockHeader *)current);
        current += WORDS_IN_BLOCK;
    }
    LargeAllocator_Sweep(heap->largeAllocator);

    if (!Allocator_CanInitCursors(heap->allocator) || Allocator_ShouldGrow(heap->allocator)) {
        size_t increment = heap->smallHeapSize / WORD_SIZE * GROWTH_RATE / 100;
        increment = (increment - 1 + WORDS_IN_BLOCK) / WORDS_IN_BLOCK * WORDS_IN_BLOCK;
        Heap_Grow(heap, increment);
    }
    Allocator_InitCursors(heap->allocator);
}
```

The lists and counts are wiped first, then rebuilt entirely from a linear
walk over **every block address** in the small heap (`current +=
WORDS_IN_BLOCK` each step) — this is the only place the whole small heap
gets walked block-by-block; everything else navigates via the free/recycled
lists or the mark worklist instead. After the small-heap walk, the large
heap gets its own separate sweep. Then: grow if either the heap genuinely
can't prime its cursors afterward, or the growth heuristic says to (see
[Allocator.md](Allocator.md) for both checks) — note the growth amount is
computed from `smallHeapSize` (the heap's size *before* this round of
recycling), `GROWTH_RATE` is 30 (%), and the result gets rounded up to a
whole number of blocks. Finally, re-prime the cursors for the next burst
of allocation.

## `Heap_Grow` / `Heap_GrowLarge`

Both just extend `heapEnd`/`largeHeapEnd` further into the
already-`mmap`'d region from `Heap_mapAndAlign` — no new `mmap` call,
since the whole physical-memory-sized region was reserved up front. `Grow`
hands the newly-extended range straight to `freeBlocks` via
`BlockList_AddBlocksLast` (an O(1) append of an already-internally-chained
run, see [datastructures/BlockList.md](datastructures/BlockList.md)).
`GrowLarge` additionally has to grow the large heap's `Bitmap` (since it's
sized to the heap's current address range) and rounds its increment up to
a power of two first, to stay consistent with the buddy-style chunk sizing
`LargeAllocator` expects everywhere else.
