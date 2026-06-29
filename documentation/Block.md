# `Block.h` / `Block.c`

This is the other half of the overflow/allocation story: where the holes
that `Allocator_nextLineRecycled` later walks actually get built. It only
ever runs as part of `Heap_Recycle`, right after a full mark phase — i.e.
it's a sweep, one block at a time, over the *entire* small heap.

## `Block_Recycle` — the per-block decision tree

```c
void Block_Recycle(Allocator *allocator, BlockHeader *blockHeader) {
    if (!Block_IsMarked(blockHeader)) {
        Block_recycleUnmarkedBlock(allocator, blockHeader);
        allocator->freeBlockCount++;
        allocator->freeMemoryAfterCollection += BLOCK_TOTAL_SIZE;
    } else {
        Block_Unmark(blockHeader);
        /* ...line-by-line walk, see below... */
    }
}
```

The very first check is the block-level summary mark
(`BlockHeader.header.mark`, set by `Object_Mark` whenever *any* object
inside this block was found reachable). If it's still unset, nothing in
this block survived — the entire block can be discarded in one shot
without looking at a single line:

```c
INLINE void Block_recycleUnmarkedBlock(Allocator *allocator, BlockHeader *blockHeader) {
    memset(blockHeader, 0, LINE_SIZE);          // clear the metadata region
    BlockList_AddLast(&allocator->freeBlocks, blockHeader);
    Block_SetFlag(blockHeader, block_free);
}
```

(`memset(..., LINE_SIZE)` clears exactly the metadata region —
`BLOCK_METADATA_ALIGNED_SIZE` is 256 bytes, one `LINE_SIZE` — wiping
`mark`/`flags`/`first`/`nextBlock` and all 127 line headers back to zero
in one call, far cheaper than the line-by-line path below.)

## If the block *is* marked: line-by-line

If `BlockHeader.header.mark` was set, somewhere in this block at least one
object survived — but most lines in it might still be dead. This is where
Immix's actual per-line (not per-object, not per-block) recycling
granularity shows up:

```c
int16_t lineIndex = 0;
int lastRecyclable = NO_RECYCLABLE_LINE;     // -1

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
```

**Marked lines** (`Block_recycleMarkedLine`) get unmarked, and — only if
the line actually contains an object header — every object in that line
gets resolved from ambiguous to definite: `marked → allocated` (it
survived, keep it as live data) or `allocated → free` (it was never
visited this trace, it's garbage):

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

Notice this per-object walk only happens for lines that are *marked* — a
line that's entirely garbage never gets this treatment at all, because the
whole line becomes a hole regardless of what dead bytes happen to still say
in it (see the `else` branch below). This is the concrete payoff of
line-granularity tracking: per-object bookkeeping only happens on the
"boundary" lines that mix live and dead objects, not across the whole
heap.

**Unmarked lines** are where holes get built and coalesced:

```c
if (lastRecyclable == NO_RECYCLABLE_LINE) {
    blockHeader->header.first = lineIndex;          // first hole found -> head of the list
} else {
    Block_GetFreeLineHeader(blockHeader, lastRecyclable)->next = lineIndex;  // chain previous hole to this one
}
lastRecyclable = lineIndex;
lineIndex++;
Line_SetEmpty(lineHeader);
allocator->freeMemoryAfterCollection += LINE_SIZE;

uint8_t size = 1;
while (lineIndex < LINE_COUNT &&
       !Line_IsMarked(lineHeader = Block_GetLineHeader(blockHeader, lineIndex))) {
    size++;
    lineIndex++;
    Line_SetEmpty(lineHeader);
    allocator->freeMemoryAfterCollection += LINE_SIZE;
}
Block_GetFreeLineHeader(blockHeader, lastRecyclable)->size = size;
```

Walk forward greedily while lines stay unmarked, counting how many
consecutive lines this hole spans (`size`), clearing each to
`line_empty` as it goes, then write that count into the
`FreeLineHeader` sitting at the *start* of the hole. `lastRecyclable`
tracks the previous hole so its `.next` can be wired to this new one —
building the same in-place linked list that `Allocator_nextLineRecycled`
walks later, one hole at a time, coalesced into the longest contiguous
runs possible.

## After the full line walk: was there anything to give back?

```c
if (lastRecyclable == NO_RECYCLABLE_LINE) {
    Block_SetFlag(blockHeader, block_unavailable);
} else {
    Block_GetFreeLineHeader(blockHeader, lastRecyclable)->next = LAST_HOLE;
    Block_SetFlag(blockHeader, block_recyclable);
    BlockList_AddLast(&allocator->recycledBlocks, blockHeader);
    allocator->recycledBlockCount++;
}
```

If every single line in the block turned out to be marked (no holes were
ever started), `lastRecyclable` never moved off `NO_RECYCLABLE_LINE` — the
block is `block_unavailable`: completely full of survivors, nothing to
hand the allocator, but not free either. It just sits there, neither in
`freeBlocks` nor `recycledBlocks`, until the *next* collection might open
up some space in it. Otherwise, terminate the hole list with `LAST_HOLE`,
flag the block recyclable, and append it to `recycledBlocks`.

`allocator->freeMemoryAfterCollection` — accumulated in both branches
above (whole blocks and individual holes) — is exactly the number
`Allocator_ShouldGrow` checks against the heap's total size (see
[Allocator.md](Allocator.md)).
