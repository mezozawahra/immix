# `Allocator.h` / `Allocator.c`

This is the bump-pointer allocator for everything that lives in the normal
32KB-block heap (i.e. everything *except* objects ≥ 8KB, which go through
[`LargeAllocator`](LargeAllocator.md) instead). It's also the single file
where the multithreading split matters most: roughly two-thirds of this
struct is global, shared bookkeeping, and one-third is the genuinely
per-mutator fast path.

> **Naming collision, read this first:** this struct has fields called
> `largeBlock`/`largeCursor`/`largeLimit`. These have **nothing to do**
> with `LargeAllocator`. They're the bump-pointer pair for **overflow
> allocation** — medium-sized objects that don't fit Immix's normal
> line-by-line path but are still well under the 8KB threshold that would
> route them to the actual large-object heap. Keep this distinction in
> mind for the rest of this document; everywhere "large" appears below it
> means "overflow," not "the `LargeAllocator`."

## The struct

```c
typedef struct {
    word_t *heapStart;
    uint64_t blockCount;

    BlockList recycledBlocks;
    uint64_t recycledBlockCount;
    BlockList freeBlocks;
    uint64_t freeBlockCount;

    BlockHeader *block;
    word_t *cursor;
    word_t *limit;

    BlockHeader *largeBlock;
    word_t *largeCursor;
    word_t *largeLimit;

    size_t freeMemoryAfterCollection;
} Allocator;
```

Two groups of fields with very different lifetimes:

- **Global, shared across the whole heap:** `heapStart`, `blockCount`,
  `recycledBlocks`/`recycledBlockCount`, `freeBlocks`/`freeBlockCount`,
  `freeMemoryAfterCollection`. These describe the pool of blocks available
  to hand out and only change when a block changes ownership (handed to a
  cursor, or returned during recycling) — this is the part that needs a
  lock if multiple mutators allocate concurrently.
- **Per-mutator fast path:** `block`/`cursor`/`limit` (normal allocation)
  and `largeBlock`/`largeCursor`/`largeLimit` (overflow allocation). These
  are touched on *every single allocation* and never need a lock as long
  as each mutator has its own copy — this is the part that should stay
  exactly as-is if/when this becomes a multi-threaded allocator with one
  `Allocator` per thread, both pulling blocks from a shared pool.

## Setup: `Allocator_Create`

Maps the entire initial heap region into one giant `freeBlocks` list —
`first = heapStart`, and every block implicitly chained to the next
(relying on the "index 0 means physically next block" convention in
[`BlockList`](datastructures/BlockList.md)), with the very last block's
`nextBlock` explicitly set to `LAST_BLOCK`. Then calls
`Allocator_InitCursors`.

## `Allocator_CanInitCursors` — why you need *two* spare blocks

```c
bool Allocator_CanInitCursors(Allocator *allocator) {
    return allocator->freeBlockCount >= 2 ||
           (allocator->freeBlockCount == 1 && allocator->recycledBlockCount > 0);
}
```

The normal cursor and the overflow cursor are both live at the same time
and must never be pointed at the same block — see why under "Why overflow
allocation always takes a *free* block" below. So initializing cursors
needs at minimum one block for each: either two free blocks, or one free
block (guaranteed for the overflow cursor) plus at least one recycled
block (for the normal cursor, which prefers recycled blocks anyway). If
neither condition holds, the heap genuinely cannot make further progress
without growing or collecting — `Heap_Grow`/`Heap_Recycle` check this
exact function.

## `Allocator_InitCursors`

```c
void Allocator_InitCursors(Allocator *allocator) {
    allocator->block = NULL;
    allocator->cursor = NULL;
    allocator->limit = NULL;
    Allocator_getNextLine(allocator);     // primes the normal cursor

    BlockHeader *largeHeader = BlockList_RemoveFirstBlock(&allocator->freeBlocks);
    allocator->largeBlock = largeHeader;
    allocator->largeCursor = Block_GetFirstWord(largeHeader);
    allocator->largeLimit = Block_GetBlockEnd(largeHeader);
}
```

The normal cursor is primed indirectly through `Allocator_getNextLine`
(covered below), which will prefer a recycled block if one's available.
The overflow cursor is primed **directly and unconditionally** from
`freeBlocks` — it never even considers a recycled block. This isn't a
matter of taste; the next section explains why that has to be true for
correctness, not just efficiency.

## The fast path: `Allocator_Alloc`

```c
INLINE word_t *Allocator_Alloc(Allocator *allocator, size_t size) {
    word_t *start = allocator->cursor;
    word_t *end = (word_t *)((uint8_t *)start + size);

    if (end > allocator->limit) {
        if (size > LINE_SIZE) {
            return Allocator_overflowAllocation(allocator, size);
        } else {
            if (Allocator_getNextLine(allocator)) {
                return Allocator_Alloc(allocator, size);
            }
            return NULL;
        }
    }

    if (end == allocator->limit) {
        memset(start, 0, size);
    } else {
        memset(start, 0, size + WORD_SIZE);
    }

    allocator->cursor = end;
    Line_Update(allocator->block, start);
    return start;
}
```

This is the only function called on every allocation, and it's a classic
bump-and-compare: if the new cursor would land at or before `limit`, zero
the memory, advance the cursor, record line bookkeeping, done. The
interesting part is what happens when it *doesn't* fit:

- **If the object is bigger than one line (`size > LINE_SIZE`, 256 bytes):**
  don't even try to find room within the *current* hole — hand it off to
  `Allocator_overflowAllocation` instead. This is "overflow allocation":
  the term specifically means *routing objects that span more than one
  line to a dedicated bump pointer backed only by fresh free blocks*,
  rather than trying to make them fit through the line-by-line hole-hopping
  path below.
- **If the object is one line or smaller:** the current hole is simply
  exhausted (not that the object is too big in general) — call
  `Allocator_getNextLine` to advance to the next hole/block and retry the
  exact same allocation. If there's no next line at all, return `NULL`
  (the caller, `Heap_AllocSmall`, will trigger a collection and retry).

**The "zero one extra word" trick:** notice the `end == limit` vs.
`end < limit` branch. When the object exactly fills the remaining space,
only `size` bytes are zeroed. Otherwise, `size + WORD_SIZE` is zeroed —
one extra word *past* the object that was just allocated. This guarantees
that immediately after any object that doesn't end exactly at a hole's
boundary, the very next word's `ObjectHeader.size` field reads as `0`.
That zero is what lets `Object_NextObject` (see [Object.md](Object.md))
terminate a forward walk through a line's objects without needing a
separate "have I reached the end of live data" check — it just keeps
calling `Object_NextObject` until it gets a `size` of zero or steps past
the line boundary it was given.

## Why overflow allocation always takes a *free* block, never a recycled one

```c
word_t *Allocator_overflowAllocation(Allocator *allocator, size_t size) {
    word_t *start = allocator->largeCursor;
    word_t *end = (word_t *)((uint8_t *)start + size);

    if (end > allocator->largeLimit) {
        if (BlockList_IsEmpty(&allocator->freeBlocks)) {
            return NULL;
        }
        BlockHeader *block = BlockList_RemoveFirstBlock(&allocator->freeBlocks);
        allocator->largeBlock = block;
        allocator->largeCursor = Block_GetFirstWord(block);
        allocator->largeLimit = Block_GetBlockEnd(block);
        return Allocator_overflowAllocation(allocator, size);
    }
    /* ...same zero-fill-one-extra-word + Line_Update as Allocator_Alloc... */
    allocator->largeCursor = end;
    Line_Update(allocator->largeBlock, start);
    return start;
}
```

Structurally this is the same bump-and-retry shape as `Allocator_Alloc`,
just always pulling from `freeBlocks` directly (never `recycledBlocks`)
when the current overflow block runs out. Two independent reasons this
matters:

1. **Guaranteed contiguous space, no fragmentation risk.** A recycled
   block's available space is already broken into holes shaped by whatever
   survived the last collection — there's no guarantee any single hole is
   big enough for a multi-line object, and hunting for a big-enough hole
   would mean walking the free-line list speculatively. A *free* block is
   one single, known-large contiguous span from byte 0 — trivially big
   enough for anything under `LARGE_BLOCK_SIZE`.
2. **The two cursors never compete for the same holes.** If overflow
   allocation could also pull from `recycledBlocks`, it would be racing the
   *normal* cursor for the exact same holes in the exact same blocks,
   which is both a correctness headache (whoever updates `block->header.first`
   last wins, the other's view of available holes goes stale) and removes
   the "this hole I just got is mine alone" property the bump pointers rely
   on with no locking, in a single-mutator design. Keeping overflow strictly
   on `freeBlocks` means the two cursors are always working in physically
   disjoint memory.

## Advancing the normal cursor: three layers

```
Allocator_getNextLine
  └─ if cursor is NULL, or the block backing it is now flagged block_free:
        Allocator_getNextBlock          (need an entirely new block)
        Allocator_firstLineNewBlock     (set up cursor/limit for it)
  └─ else:
        Allocator_nextLineRecycled      (just hop to this block's next hole)
```

- **`Allocator_getNextBlock`** — `recycledBlocks` first, `freeBlocks` as
  fallback, `NULL` if both are empty. Recycled-first is a deliberate Immix
  policy: use up the holes in already-partially-full blocks before opening
  a brand new block, which keeps live data denser and improves locality.

- **`Allocator_firstLineNewBlock`** — branches on whether the new block is
  free (`cursor = Block_GetFirstWord`, `limit = Block_GetBlockEnd` — the
  *whole* block is fair game) or recycled (read `block->header.first` to
  find its first hole, same as below).

- **`Allocator_nextLineRecycled`** — walks the in-block free-line list
  (see [headers/LineHeader.md](headers/LineHeader.md)): read
  `block->header.first`'s `FreeLineHeader` to get this hole's `size`, set
  `limit = line + size * WORDS_IN_LINE`, and advance
  `block->header.first = lineHeader->next` so the *next* call finds the
  next hole. If `header.first == LAST_HOLE`, this block has no more holes
  left — fall back to `Allocator_getNextLine` again to get a fresh block
  entirely.

  One subtlety in `Allocator_getNextLine`'s own check: it decides "do I
  need a whole new block" by testing `cursor == NULL` **or** the current
  block is flagged `block_free`. A block being flagged free (rather than
  recyclable) is exactly what tells this code "I just bump-allocated
  through this entire block start-to-end with no holes to hop between" —
  there's nothing left to call `Allocator_nextLineRecycled` on, the whole
  thing is spent. A *recyclable* block, by contrast, might still have more
  holes after the current one, which is precisely what
  `Allocator_nextLineRecycled` checks for via `header.first`.

## `Allocator_ShouldGrow` — the three-part growth heuristic

```c
bool Allocator_ShouldGrow(Allocator *allocator) {
    uint64_t unavailableBlockCount =
        allocator->blockCount - (allocator->freeBlockCount + allocator->recycledBlockCount);

    return allocator->freeBlockCount < allocator->blockCount / 3 ||
           4 * unavailableBlockCount > allocator->blockCount ||
           allocator->freeMemoryAfterCollection * 2 < allocator->blockCount * BLOCK_TOTAL_SIZE;
}
```

Three independent red flags, any one of which triggers growth (checked
right after a collection, in `Heap_Recycle`):

1. **`freeBlockCount < blockCount / 3`** — fewer than a third of all blocks
   are virgin/fully-free. Running low on the "cheap, no-hole-hunting-needed"
   tier of memory.
2. **`4 * unavailableBlockCount > blockCount`** — more than a quarter of
   all blocks are completely packed with survivors (no holes at all). A
   rising fraction of fully-unavailable blocks is a sign the live set is
   growing relative to the heap.
3. **`freeMemoryAfterCollection * 2 < blockCount * BLOCK_TOTAL_SIZE`** —
   the last collection reclaimed less than half the heap's total bytes.
   Low yield from collection means the next collection probably won't buy
   much breathing room either, so it's cheaper to just grow now.
