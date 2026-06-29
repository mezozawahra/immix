# `LargeAllocator.h` / `LargeAllocator.c`

A completely separate heap from everything in [Allocator.md](Allocator.md)
— different memory region (`heap->largeHeapStart`/`largeHeapEnd`, mapped
independently in `Heap_Create`), different data structures, used only for
objects whose total size (header included) is `>= LARGE_BLOCK_SIZE` (8KB).
It's a power-of-two, buddy-style allocator, but a simplified one: chunks
aren't tracked in buddy pairs, so there's no "free this chunk, check if its
buddy is also free, merge" logic on every free. Coalescing only happens in
bulk, once per collection, during `LargeAllocator_Sweep`.

## The data structures

```c
typedef struct Chunk {
    ObjectHeader header;
    Chunk *next;
} Chunk;

typedef struct { Chunk *first; Chunk *last; } FreeList;

typedef struct {
    word_t *offset;
    size_t size;
    FreeList freeLists[FREE_LIST_COUNT];   // one per power-of-two size class
    Bitmap *bitmap;
} LargeAllocator;
```

A `Chunk` is, deliberately, also a valid `Object` — it has a real
`ObjectHeader` with `type = object_large` and `flag = object_free`. This
means the exact same sweep/walk code that processes live large objects
(`Object_NextLargeObject`, `Object_ChunkSize`) can also walk *free* chunks
without any special-casing — a chunk is just an object that happens to be
flagged free, not a structurally different thing.

`freeLists[FREE_LIST_COUNT]` — `FREE_LIST_COUNT` is `34 - 13 + 1 = 22`
(see [Constants.md](Constants.md)): one singly-linked free list per
power-of-two size from `MIN_BLOCK_SIZE` (8KB) up to `MAX_BLOCK_SIZE` (16GB).

`bitmap` — one bit per `MIN_BLOCK_SIZE`-aligned (8KB) address across the
*entire* large-heap range, set wherever a chunk (free **or** allocated)
currently starts. See [datastructures/Bitmap.md](datastructures/Bitmap.md).

## Splitting a byte range into power-of-two chunks: `LargeAllocator_AddChunk`

```c
void LargeAllocator_AddChunk(LargeAllocator *allocator, Chunk *chunk, size_t total_block_size) {
    size_t remaining_size = total_block_size;
    ubyte_t *current = (ubyte_t *)chunk;
    while (remaining_size > 0) {
        int log2_f = log2_floor(remaining_size);
        size_t chunkSize = 1UL << log2_f;
        chunkSize = chunkSize > MAX_BLOCK_SIZE ? MAX_BLOCK_SIZE : chunkSize;
        int listIndex = LargeAllocator_sizeToLinkedListIndex(chunkSize);

        LargeAllocator_freeListAddBlockLast(&allocator->freeLists[listIndex], (Chunk *)current);
        LargeAllocator_setChunkSize((Chunk *)current, chunkSize);
        ((Chunk *)current)->header.type = object_large;
        Object_SetFree(&((Object *)current)->header);
        Bitmap_SetBit(allocator->bitmap, current);

        current += chunkSize;
        remaining_size -= chunkSize;
    }
}
```

Given an arbitrary byte range, greedily carve off the *largest*
power-of-two chunk that fits (capped at `MAX_BLOCK_SIZE`), register it in
its size class's free list, stamp it as a free chunk, set its bitmap bit,
and repeat with whatever's left. This is the classic "decompose a range
into powers of two" loop (the same idea as representing a number in
binary). It's called in three places: once at heap-creation time (one
giant initial chunk covering the whole large heap), once whenever
`LargeAllocator_GetBlock` has carved out a request and has a leftover
remainder to register, and once per dead run during
`LargeAllocator_Sweep`.

## Getting a block: `LargeAllocator_GetBlock`

```c
Object *LargeAllocator_GetBlock(LargeAllocator *allocator, size_t requestedBlockSize) {
    size_t actualBlockSize = MathUtils_RoundToNextMultiple(requestedBlockSize, MIN_BLOCK_SIZE);
    size_t requiredChunkSize = 1UL << MathUtils_Log2Ceil(actualBlockSize);

    int listIndex = LargeAllocator_sizeToLinkedListIndex(requiredChunkSize);
    Chunk *chunk = NULL;
    while (listIndex <= FREE_LIST_COUNT - 1 &&
           (chunk = allocator->freeLists[listIndex].first) == NULL) {
        ++listIndex;
    }
    if (chunk == NULL) return NULL;   // truly out of large memory

    size_t chunkSize = LargeAllocator_getChunkSize(chunk);
    if (chunkSize - MIN_BLOCK_SIZE >= actualBlockSize) {
        // big enough to be worth splitting: carve off the request, re-add the remainder
        Chunk *remainingChunk = LargeAllocator_chunkAddOffset(chunk, actualBlockSize);
        LargeAllocator_freeListRemoveFirstBlock(&allocator->freeLists[listIndex]);
        LargeAllocator_AddChunk(allocator, remainingChunk, chunkSize - actualBlockSize);
    } else {
        // remainder too small to bother splitting off; hand out the whole chunk
        LargeAllocator_freeListRemoveFirstBlock(&allocator->freeLists[listIndex]);
    }

    Bitmap_SetBit(allocator->bitmap, (ubyte_t *)chunk);
    Object *object = (Object *)chunk;
    Object_SetAllocated(&object->header);
    memset(Object_ToMutatorAddress(object), 0, actualBlockSize - WORD_SIZE);
    return object;
}
```

Three steps:

1. Round the request up to a whole number of `MIN_BLOCK_SIZE` units, then
   round *that* up further to the next power of two — large chunks are
   always handed out as whole power-of-two units, never an arbitrary
   multiple.
2. Scan free lists starting at exactly that size class and moving
   *upward* until a non-empty one is found — this is first-fit by size
   class (not first-fit across all chunks, and not best-fit): any chunk
   big enough is accepted, even if a same-class chunk would've been a
   tighter fit, because the request already determined which (or a larger)
   class to look in.
3. If the chunk found is bigger than strictly necessary by at least one
   more `MIN_BLOCK_SIZE` unit, split off the exact amount needed and
   re-register the leftover via `AddChunk` (which itself may break that
   leftover into multiple smaller power-of-two pieces if it doesn't
   round-trip into one clean class). Otherwise, the small amount of
   leftover (less than `MIN_BLOCK_SIZE`) is simply absorbed as internal
   fragmentation — not worth the bookkeeping to reclaim.

## Sweeping: where coalescing actually happens

```c
void LargeAllocator_Sweep(LargeAllocator *allocator) {
    LargeAllocator_clearFreeLists(allocator);

    Object *current = (Object *)allocator->offset;
    void *heapEnd = (ubyte_t *)allocator->offset + allocator->size;

    while (current != heapEnd) {
        if (Object_IsMarked(&current->header)) {
            Object_SetAllocated(&current->header);
            current = Object_NextLargeObject(current);
        } else {
            size_t currentSize = Object_ChunkSize(current);
            Object *next = Object_NextLargeObject(current);
            while (next != heapEnd && !Object_IsMarked(&next->header)) {
                currentSize += Object_ChunkSize(next);
                Bitmap_ClearBit(allocator->bitmap, (ubyte_t *)next);
                next = Object_NextLargeObject(next);
            }
            LargeAllocator_AddChunk(allocator, (Chunk *)current, currentSize);
            current = next;
        }
    }
}
```

Unlike `Block_Recycle`'s free lists, which only ever describe *some* of the
small heap (`freeBlocks`/`recycledBlocks` are rebuilt by walking the heap
externally, in `Heap_Recycle`), the large allocator's free lists are
**entirely thrown away and rebuilt from scratch here** — `clearFreeLists`
first, every chunk re-registered as the walk finds it. The walk itself
goes in strict address order across the whole large-heap range (this is
the *only* place that happens — `LargeAllocator_GetBlock` never needs to
walk the heap linearly, only `Sweep` does, because finding dead chunks that
might not be in any free list requires seeing every chunk, not just the
free ones).

For each chunk: if marked, it survived — demote `marked → allocated`,
unmark implicitly happens by virtue of the flag being overwritten, and
move past it via `Object_NextLargeObject`. If unmarked, it's dead — but
instead of immediately re-adding just this one chunk, keep walking forward
through however many *more* consecutive dead chunks follow, accumulating
their combined size and clearing their bitmap bits, then register the
**whole merged run** as one (possibly larger, possibly split back into
several power-of-two pieces by `AddChunk`) span. This consecutive-merge is
the actual coalescing step — it's why two adjacent small dead chunks can
become eligible to satisfy a bigger future request, even though neither
one alone was big enough.

## Size-class math

```c
inline static int LargeAllocator_sizeToLinkedListIndex(size_t size) {
    return log2_floor(size) - LARGE_OBJECT_MIN_SIZE_BITS;
}
```

Straightforward: `log2_floor` of a power-of-two size returns the exact
exponent, and subtracting `LARGE_OBJECT_MIN_SIZE_BITS` (13) rebases it so
index `0` corresponds to `MIN_BLOCK_SIZE` (8KB).

`LargeAllocator_getChunkSize`/`_setChunkSize` reuse `ObjectHeader.size`
exactly the way `Object_Size`/`Object_SetSize` do (shifted by
`WORD_SIZE_BITS`) — a `Chunk`'s size lives in the same field a real
object's size would, since a `Chunk` *is* an `ObjectHeader`-prefixed
struct.
