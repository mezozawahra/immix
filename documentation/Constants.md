# `Constants.h`

All of the fixed geometry of the heap lives here as preprocessor constants.
Nothing in this file has logic — it's worth reading once with a calculator,
because every other file leans on these numbers being exactly what they are.

## Word size

```c
#define WORD_SIZE_BITS 3
#define WORD_SIZE (1 << WORD_SIZE_BITS)        // 8 bytes
#define WORD_INVERSE_MASK (~((word_t)WORD_SIZE - 1))
```

`WORD_SIZE` is 8 bytes (pointer-sized on a 64-bit target). `WORD_INVERSE_MASK`
rounds any address *down* to the nearest word boundary when ANDed with it —
used in `Object.c` to fix up a misaligned conservative root before trying to
resolve it to an object.

## Block geometry

```c
#define BLOCK_SIZE_BITS 15
#define BLOCK_TOTAL_SIZE (1 << BLOCK_SIZE_BITS)   // 32768 bytes (32KB)
```

A block is the unit the Immix allocator hands out and recycles. 32KB blocks
are always mapped at a 32KB-aligned address (`Heap_mapAndAlign` in
`Heap.c` guarantees this), which is what lets `Block_GetBlockHeader` recover
a block's start from *any* interior address with a single AND — no lookup
table, no traversal:

```c
BlockHeader *Block_GetBlockHeader(word_t *word) {
    return (BlockHeader *)((word_t)word & BLOCK_SIZE_IN_BYTES_INVERSE_MASK);
}
```

## Line geometry

```c
#define LINE_SIZE_BITS 8
#define LINE_SIZE (1UL << LINE_SIZE_BITS)        // 256 bytes
```

A line is the granularity at which Immix tracks liveness and recycles holes.
256 bytes is small enough that fragmentation inside a block stays low, large
enough that the per-line bookkeeping (one byte of `LineHeader` per line,
see [headers/LineHeader.md](headers/LineHeader.md)) is cheap.

## Per-block / per-line metadata size

```c
#define BLOCK_METADATA_SIZE_BITS 3
#define LINE_METADATA_SIZE_BITS 0
#define BLOCK_METADATA_SIZE (1 << BLOCK_METADATA_SIZE_BITS)  // 8 bytes
#define LINE_METADATA_SIZE (1 << LINE_METADATA_SIZE_BITS)    // 1 byte
```

`BLOCK_METADATA_SIZE` (8 bytes) is the size of the *fixed* part of
`BlockHeader` — its `{ mark, flags, first, nextBlock }` struct, which is
exactly `1+1+2+4 = 8` bytes. `LINE_METADATA_SIZE` (1 byte) is the size of a
single `LineHeader`.

## How many lines actually fit in a block

```c
#define LINE_COUNT \
    ((BLOCK_TOTAL_SIZE - BLOCK_METADATA_SIZE) / (LINE_SIZE + LINE_METADATA_SIZE))
```

This is solving "how many (line + its 1-byte header) pairs fit in the space
left over after the fixed 8-byte block header" — but notice the line
*headers* and the line *data* aren't actually interleaved in memory (the
data structure is `lineHeaders[LINE_COUNT]` followed later by the actual
line bytes, not header-then-line-then-header-then-line). This formula is
really just finding the largest `LINE_COUNT` such that everything still fits
in one 32KB block once you account for one header byte per line: `(32768 -
8) / 257 = 127` (integer division). With `LINE_COUNT = 127`:

```c
#define TOTAL_BLOCK_METADATA_SIZE (BLOCK_METADATA_SIZE + (LINE_COUNT * LINE_METADATA_SIZE))
// 8 + 127*1 = 135 bytes

#define BLOCK_METADATA_ALIGNED_SIZE \
    ((TOTAL_BLOCK_METADATA_SIZE + LINE_SIZE - 1) / LINE_SIZE * LINE_SIZE)
// round 135 up to the next multiple of 256 -> 256 bytes
```

So in practice: the first 256 bytes of every block (exactly one line's
worth) is metadata (`BlockHeader`'s 8-byte header plus all 127
`LineHeader` bytes, with some padding), and the remaining `32768 - 256 =
32512` bytes is exactly `127 * 256` — i.e. exactly `LINE_COUNT` real,
usable 256-byte lines. The arithmetic is self-consistent by construction:
whatever `LINE_COUNT` the first formula produces, the data region is always
an exact multiple of it.

```c
#define WORDS_IN_LINE (LINE_SIZE / WORD_SIZE)     // 32
#define WORDS_IN_BLOCK (BLOCK_TOTAL_SIZE / WORD_SIZE)  // 4096
```

## Block alignment masks

```c
#define BLOCK_SIZE_IN_BYTES_MASK (BLOCK_TOTAL_SIZE - 1)
#define BLOCK_SIZE_IN_BYTES_INVERSE_MASK (~BLOCK_SIZE_IN_BYTES_MASK)
```

Standard power-of-two alignment masks: `MASK` extracts the offset *within*
a block, `INVERSE_MASK` extracts the block's own base address.

## Large object thresholds (the *other* heap)

```c
#define LARGE_BLOCK_SIZE_BITS 13
#define LARGE_BLOCK_SIZE (1 << LARGE_BLOCK_SIZE_BITS)   // 8192 bytes (8KB)

#define LARGE_OBJECT_MIN_SIZE_BITS 13
#define LARGE_OBJECT_MAX_SIZE_BITS 34
#define MIN_BLOCK_SIZE (1UL << LARGE_OBJECT_MIN_SIZE_BITS)  // 8KB
#define MAX_BLOCK_SIZE (1UL << LARGE_OBJECT_MAX_SIZE_BITS)  // 16GB

#define LARGE_BLOCK_MASK (~((1 << LARGE_OBJECT_MIN_SIZE_BITS) - 1))
```

`LARGE_BLOCK_SIZE` (8KB) is the cutoff `Heap_Alloc` uses to decide whether
an object goes through the normal Immix block allocator or through the
separate `LargeAllocator` heap entirely (see [Heap.md](Heap.md) and
[LargeAllocator.md](LargeAllocator.md) — and note the **naming collision**
called out in the top-level README: this `LARGE_BLOCK_SIZE` constant is
about the `LargeAllocator` heap, while `Allocator`'s own `largeCursor`
field is something else entirely — "overflow" allocation inside the normal
heap).

`MIN_BLOCK_SIZE`/`MAX_BLOCK_SIZE` define the range of power-of-two chunk
sizes the `LargeAllocator`'s buddy-style free lists cover (8KB up to 16GB —
`FREE_LIST_COUNT` in `LargeAllocator.h` is `34 - 13 + 1 = 22` size classes).
`LARGE_BLOCK_MASK` rounds any address down to an 8KB boundary — used
throughout `Object.c`/`LargeAllocator.c` because every large-heap chunk is
naturally aligned to (at least) 8KB.

## Growth

```c
#define GROWTH_RATE 30
```

When the heap needs to grow (see `Heap_Grow` in [Heap.md](Heap.md)), it
grows by 30% of the *current* small-heap size, rounded up to a whole number
of blocks.
