# `Block.h` / `Block.c`

This is the other half of the overflow/allocation story: where the holes
that `ThreadLocalAllocator_nextLineRecycled` later walks actually get
built. It only ever runs as part of `Heap_Recycle`, right after a full
mark phase — i.e. it's a sweep, one block at a time, over the *entire*
small heap, and (post-multithreading) it only ever runs while every
mutator thread is parked (see [Heap.md](Heap.md)'s `Heap_Collect`/
`Heap_Recycle`), so nothing in here needs its own locking around the
per-line bookkeeping it mutates directly.

## What changed from the single-mutator version

`Block_Recycle` used to take an `Allocator *` and write straight into its
`freeBlocks`/`recycledBlocks` lists and counters. Now that those lists
live behind the shared, locked
[`GlobalBlockAllocator`](concurrent/GlobalBlockAllocator.md) (since
multiple mutator threads each have their own
[`ThreadLocalAllocator`](ThreadLocalAllocator.md) pulling from one shared
pool), `Block_Recycle` takes a `GlobalBlockAllocator *` instead and
reports results through its locked API — `GlobalBlockAllocator_AddFreeBlock`,
`GlobalBlockAllocator_AddRecycledBlock`, `GlobalBlockAllocator_AddFreeMemory` —
rather than touching any list field directly. The actual per-line
algorithm (deciding which lines are holes, coalescing runs, building the
in-block free-line list) is **byte-for-byte unchanged**; only where the
results get reported changed.

```c
void Block_Recycle(GlobalBlockAllocator *globalAllocator, BlockHeader *blockHeader);
```

## `Block_Recycle` — the per-block decision tree

```c
void Block_Recycle(GlobalBlockAllocator *globalAllocator, BlockHeader *blockHeader) {
    if (!Block_IsMarked(blockHeader)) {
        Block_recycleUnmarkedBlock(globalAllocator, blockHeader);
        GlobalBlockAllocator_AddFreeMemory(globalAllocator, BLOCK_TOTAL_SIZE);
        return;
    }

    assert(Block_IsMarked(blockHeader));
    Block_Unmark(blockHeader);
    /* ...line-by-line walk, see below... */
}
```

The very first check is still the block-level summary mark
(`BlockHeader.header.mark`, set by `Object_Mark` whenever *any* object
inside this block was found reachable — see
[headers/ObjectHeader.md](headers/ObjectHeader.md)). If it's still unset,
nothing in this block survived — the entire block can be discarded in one
shot without looking at a single line:

```c
INLINE void Block_recycleUnmarkedBlock(GlobalBlockAllocator *globalAllocator,
                                       BlockHeader *blockHeader) {
    memset(blockHeader, 0, LINE_SIZE);
    GlobalBlockAllocator_AddFreeBlock(globalAllocator, blockHeader);
    Block_SetFlag(blockHeader, block_free);
}
```

(`memset(..., LINE_SIZE)` still clears exactly the 256-byte metadata
region in one call, same as before — wiping `mark`/`flags`/`first`/
`nextBlock` and all 127 `LineHeader` bytes to zero.
`GlobalBlockAllocator_AddFreeBlock` replaces what used to be a direct
`BlockList_AddLast(&allocator->freeBlocks, blockHeader)` plus a separate
`allocator->freeBlockCount++` — the global allocator's version does both
atomically under its lock.)

## If the block *is* marked: line-by-line, with batched reporting

```c
int16_t lineIndex = 0;
int lastRecyclable = NO_RECYCLABLE_LINE;
size_t freedInBlock = 0;

while (lineIndex < LINE_COUNT) {
    LineHeader *lineHeader = Block_GetLineHeader(blockHeader, lineIndex);

    if (Line_IsMarked(lineHeader)) {
        Block_recycleMarkedLine(blockHeader, lineHeader, lineIndex);
        lineIndex++;
    } else {
        /* start or extend a hole, coalescing consecutive unmarked lines */
        ...
    }
}

if (freedInBlock > 0) {
    GlobalBlockAllocator_AddFreeMemory(globalAllocator, freedInBlock);
}
```

This is the one deliberate change beyond "swap direct field writes for
locked calls." In the original, every individual unmarked line
immediately did `allocator->freeMemoryAfterCollection += LINE_SIZE` —
a direct field write, free because there was only one allocator and one
thread. Doing that now would mean acquiring `GlobalBlockAllocator`'s lock
once *per recycled line*, for the entire small heap, on every collection —
needless contention for a counter that nothing reads until the whole
sweep is done. Instead, `freedInBlock` accumulates locally across the
*entire* line walk for this one block, and gets reported in a single
`GlobalBlockAllocator_AddFreeMemory` call at the very end — one lock/unlock
per block instead of one per recycled line. `Block_recycleUnmarkedBlock`'s
single `BLOCK_TOTAL_SIZE` report (above) needs no such batching since it's
already exactly one call per block.

**Marked lines** (`Block_recycleMarkedLine`) — entirely unchanged from the
single-mutator version, since this part never touched the allocator at
all: unmark the line, and — only if it actually contains an object header —
resolve every object in it from ambiguous to definite, `marked → allocated`
(survived) or `allocated → free` (never visited, garbage):

```c
INLINE void Block_recycleMarkedLine(BlockHeader *blockHeader, LineHeader *lineHeader, int lineIndex) {
    Line_Unmark(lineHeader);
    if (Line_ContainsObject(lineHeader)) {
        Object *object = Line_GetFirstObject(lineHeader);
        word_t *lineEnd = Block_GetLineAddress(blockHeader, lineIndex) + WORDS_IN_LINE;
        while (object != NULL && (word_t *)object < lineEnd) {
            if (Object_IsMarked(&object->header)) {
                Object_SetAllocated(&object->header);
            } else {
                Object_SetFree(&object->header);
            }
            object = Object_NextObject(object);
        }
    }
}
```

This per-object walk only ever runs for lines that mix live and dead
objects — a wholly-garbage line never gets this treatment, it becomes a
hole regardless of what dead bytes are sitting in it (next section). This
is still the concrete payoff of line-granularity tracking: per-object
bookkeeping is confined to the "boundary" lines, not the whole heap.

**Unmarked lines** — also unchanged algorithmically (greedy coalescing of
consecutive unmarked lines into one hole, recorded via the in-place
`FreeLineHeader` linked list — see
[headers/LineHeader.md](headers/LineHeader.md)) — except every line's
`LINE_SIZE` contribution now adds to the local `freedInBlock` accumulator
instead of writing straight into `allocator->freeMemoryAfterCollection`:

```c
lastRecyclable = lineIndex;
lineIndex++;
Line_SetEmpty(lineHeader);
freedInBlock += LINE_SIZE;
uint8_t size = 1;
while (lineIndex < LINE_COUNT &&
       !Line_IsMarked(lineHeader = Block_GetLineHeader(blockHeader, lineIndex))) {
    size++;
    lineIndex++;
    Line_SetEmpty(lineHeader);
    freedInBlock += LINE_SIZE;
}
Block_GetFreeLineHeader(blockHeader, lastRecyclable)->size = size;
```

## After the full line walk: was there anything to give back?

```c
if (lastRecyclable == NO_RECYCLABLE_LINE) {
    Block_SetFlag(blockHeader, block_unavailable);
} else {
    Block_GetFreeLineHeader(blockHeader, lastRecyclable)->next = LAST_HOLE;
    Block_SetFlag(blockHeader, block_recyclable);
    GlobalBlockAllocator_AddRecycledBlock(globalAllocator, blockHeader);

    assert(blockHeader->header.first != NO_RECYCLABLE_LINE);
}
```

Same logic as before: if every line in the block turned out marked
(`lastRecyclable` never moved), the block is `block_unavailable` —
completely full of survivors, given to no one, until the *next* collection
might open up space. Otherwise, terminate the hole list with `LAST_HOLE`,
flag the block recyclable, and report it via
`GlobalBlockAllocator_AddRecycledBlock` (replacing the old direct
`BlockList_AddLast(&allocator->recycledBlocks, blockHeader)` +
`allocator->recycledBlockCount++` pair).

`Block_Print` is unchanged — it only reads block/line state for debugging
output, never touches the allocator.