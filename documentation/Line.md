# `Line.h`

Two small functions that sit between the bump allocator and the
[`LineHeader`](headers/LineHeader.md) bookkeeping it has to keep up to
date as it carves out objects.

```c
static INLINE Object *Line_GetFirstObject(LineHeader *lineHeader) {
    assert(Line_ContainsObject(lineHeader));
    BlockHeader *blockHeader = Block_BlockHeaderFromLineHeader(lineHeader);
    uint8_t offset = Line_GetFirstObjectOffset(lineHeader);
    uint32_t lineIndex = Block_GetLineIndexFromLineHeader(blockHeader, lineHeader);
    return (Object *)Block_GetLineWord(blockHeader, lineIndex, offset / WORD_SIZE);
}
```

Reconstructs the address of "the first object that starts in this line"
purely from the line's own header: find the owning block, find this line's
index, then jump to `offset` bytes (converted to a word index) into that
line. Used by `Object_getInLine` ([Object.md](Object.md)) as the starting
point for a forward walk through a line's objects, and by
`Block_recycleMarkedLine` ([Block.md](Block.md)) to know where to start
unmarking objects in a still-live line.

```c
static INLINE void Line_Update(BlockHeader *blockHeader, word_t *objectStart) {
    int lineIndex = Block_GetLineIndexFromWord(blockHeader, objectStart);
    LineHeader *lineHeader = Block_GetLineHeader(blockHeader, lineIndex);
    if (!Line_ContainsObject(lineHeader)) {
        uint8_t offset = (uint8_t)((word_t)objectStart & LINE_SIZE_MASK);
        Line_SetOffset(lineHeader, offset);
    }
}
```

Called from both `Allocator_Alloc` and `Allocator_overflowAllocation`
(see [Allocator.md](Allocator.md)) every time a new object is carved out,
right after the bump pointer advances. It figures out which line the new
object's *start* address falls in, and — only if that line doesn't already
have a recorded first-object offset — records this object's offset as the
line's first.

The guard (`if (!Line_ContainsObject(...))`) is what makes this safe to
call unconditionally on every single allocation without corrupting earlier
bookkeeping: the first object to ever start in a line wins that line's
recorded offset permanently, until the line is recycled back to
`line_empty` and becomes eligible to record a new "first" again.
