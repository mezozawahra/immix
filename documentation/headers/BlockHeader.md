# `headers/BlockHeader.h`

Every 32KB block is self-describing: its own header, plus the header for
every one of its 127 lines, lives in the first 256 bytes of the block
itself (see [Constants.md](../Constants.md) for why it's exactly 256). No
external side-table maps a block to its metadata — you derive a block's
metadata address purely from the block's own start address.

## `BlockFlag` — what state a block is in

```c
typedef enum {
    block_free = 0x0,        // entirely empty, never touched / fully reclaimed
    block_recyclable = 0x1,  // has at least one usable hole
    block_unavailable = 0x2  // completely packed with live lines, nothing to offer
} BlockFlag;
```

`block_free` blocks are virgin or fully-reclaimed memory — the whole block
is fair game from the first byte. `block_recyclable` blocks have survivors
in some lines and holes in others; the allocator can still get more out of
them via the in-block free-line list (see below, and
[Allocator.md](../Allocator.md)). `block_unavailable` blocks are fully
alive — there's nothing left to recycle until the *next* collection might
free something in them, so the allocator skips them entirely (they're not
in `freeBlocks` or `recycledBlocks`, they're just not anywhere — the
allocator only ever sees them again at the next sweep).

## The header struct

```c
typedef struct {
    struct {
        uint8_t mark;
        uint8_t flags;
        int16_t first;
        int32_t nextBlock;
    } header;
    LineHeader lineHeaders[LINE_COUNT];
} BlockHeader;
```

- **`mark`** — set by `Object_Mark` whenever *any* object inside this block
  gets marked. This is a block-level summary bit, separate from
  individual-line marks below: `Block_Recycle` checks this first, and if
  it's still `0` after a full mark phase, the entire block is dead and gets
  recycled in one shot (`Block_recycleUnmarkedBlock`) without inspecting a
  single line.
- **`flags`** — a `BlockFlag`.
- **`first`** — the index of the first *hole* (free line, or first line of
  a run of consecutive free lines) in this block, or `LAST_HOLE` (`-1`,
  defined in `Block.h`) if there isn't one. This is the head of an
  in-place singly-linked list — see [headers/LineHeader.md](LineHeader.md)
  for how the list nodes themselves are stored *inside the dead lines'
  own bytes*, costing no extra memory.
- **`nextBlock`** — used only when this block is a node in a `BlockList`
  (`freeBlocks` / `recycledBlocks` in `Allocator`). It's a *relative block
  index* from the heap's start, not a pointer — see
  [datastructures/BlockList.md](../datastructures/BlockList.md) for why
  (short version: it survives the `memset` that clears a block's metadata
  during recycling, and it's 4 bytes instead of 8).
- **`lineHeaders[LINE_COUNT]`** — one byte per line (127 of them), stored
  inline immediately after the 8-byte fixed header, filling out the rest
  of the first 256-byte line.

## The pointer-masking trick

```c
static inline BlockHeader *Block_GetBlockHeader(word_t *word) {
    return (BlockHeader *)((word_t)word & BLOCK_SIZE_IN_BYTES_INVERSE_MASK);
}
```

Because every block is mapped at a 32KB-aligned address (guaranteed by
`Heap_mapAndAlign` in `Heap.c`), masking off the low 15 bits of *any*
address inside a block recovers that block's start address — O(1), no
traversal, no lookup table. This single function is how almost every other
file (the allocator, the marker, the recycler, the conservative pointer
resolver in `Object.c`) goes from "I have some address inside the small
heap" to "here's its `BlockHeader`."

## Line addressing

```c
static inline word_t *Block_GetLineAddress(BlockHeader *blockHeader, int lineIndex) {
    assert(lineIndex < LINE_COUNT);
    return (word_t *)((ubyte_t *)blockHeader + BLOCK_METADATA_ALIGNED_SIZE
                       + (lineIndex * LINE_SIZE));
}
```

Skip past the metadata region (`BLOCK_METADATA_ALIGNED_SIZE`, 256 bytes),
then walk `lineIndex` whole 256-byte lines forward. Everything else in this
section is a thin variant of the same idea:

- `Block_GetLineWord(block, lineIndex, wordIndex)` — one specific word
  inside a line.
- `Block_GetFreeLineHeader(block, lineIndex)` — reinterpret a line's first
  bytes as a `FreeLineHeader` (only valid when that line is actually free —
  see [headers/LineHeader.md](LineHeader.md)).
- `Block_GetLineIndexFromWord(blockHeader, word)` — the inverse of
  `Block_GetLineAddress`: `(word - firstWord) >> LINE_SIZE_BITS`.
- `Block_GetFirstWord` / `Block_GetBlockEnd` — the usable data region's
  start and (one-past-)end addresses, i.e. `Block_GetLineAddress(block, 0)`
  and `Block_GetLineAddress(block, LINE_COUNT)`.
- `Block_GetLineIndexFromLineHeader` / `Block_BlockHeaderFromLineHeader` —
  going from a `LineHeader*` back to its index or owning block, via
  pointer subtraction against `blockHeader->lineHeaders`.

## Mark/flag accessors

`Block_IsRecyclable` / `Block_IsUnavailable` / `Block_IsFree` /
`Block_SetFlag` / `Block_IsMarked` / `Block_Unmark` / `Block_Mark` are
direct reads/writes of `header.flags`/`header.mark` — no logic beyond the
comparison itself.
