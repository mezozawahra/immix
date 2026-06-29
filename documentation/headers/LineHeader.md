# `headers/LineHeader.h`

One byte, per 256-byte line, packing three independent pieces of
information — and a second struct (`FreeLineHeader`) that *isn't* metadata
at all, but a free-list node that gets written directly into a dead line's
own bytes.

## `LineHeader` — one byte, three things

```c
typedef uint8_t LineHeader;

typedef enum {
    line_empty = 0x0,
    line_marked = 0x1,
    line_contains_object_header = 0x2,
} LineFlag;

#define FIRST_OBJECT_OFFSET_MASK (uint8_t)0xFC
```

The byte is read as three overlapping pieces:

- **bit 0** (`line_marked`) — was any object in this line marked during the
  last trace? Set via `Line_Mark`, checked via `Line_IsMarked`, cleared via
  `Line_Unmark`. This is the per-line equivalent of `BlockHeader.mark`, and
  it's what `Block_Recycle` uses to decide, line by line, "keep as-is
  (still has survivors)" vs. "this is a hole now."
- **bit 1** (`line_contains_object_header`) — does *some* object's header
  start within this line? Note this is **not** the same question as "does
  this line contain live data" — a line can be the tail-end of a large
  object that started in the previous line and contain no header of its
  own (bit 1 clear), while still being marked (bit 0 set) because that
  object is alive.
- **bits 2–7** (masked by `FIRST_OBJECT_OFFSET_MASK = 0xFC`) — when bit 1
  is set, these bits hold the byte offset *within the line* of the first
  object header that starts there. Because objects are always word-aligned
  (8 bytes), the low 3 bits of any real offset are always zero anyway —
  which is exactly why they're free to reuse as the two flag bits above.
  That's the whole reason this is one combined byte instead of two
  separate fields.

```c
static inline void Line_SetOffset(LineHeader *lineHeader, uint8_t offset) {
    *lineHeader = (offset & FIRST_OBJECT_OFFSET_MASK) | line_contains_object_header;
}
static inline uint8_t Line_GetFirstObjectOffset(LineHeader *lineHeader) {
    return *lineHeader & FIRST_OBJECT_OFFSET_MASK;
}
```

`Line_SetOffset` is called from `Line_Update` (see [Line.md](../Line.md))
exactly once per line — the *first* time an object happens to start in
that line — and is never called again for that line until it's recycled
back to `line_empty`. This matters: if a second, later object also starts
in the same line, its offset must **not** overwrite the first one, because
the recorded offset is what lets conservative pointer resolution
(`Object_GetObject` in `Object.c`) find the *start* of the chain of objects
in a line and then walk forward object-by-object to whichever one actually
contains the target address. If the offset pointed at the second object,
resolving a pointer into the first object would be impossible.

Why this matters concretely: imagine a 40-byte object starting at offset 0
of a line, followed immediately by a 300-byte object that spans into the
*next* line too. The next line's own header has bit 1 clear (no object
*starts* there) but bit 0 may be set (marked, because the 300-byte object
is alive) — `Object_GetObject` resolving an address inside that next line
has to walk *backward* to the previous line that does have bit 1 set, read
its recorded offset to find the 40-byte object, then walk forward via
`Object_NextObject` until it steps past the boundary into the right
object. See [Object.md](../Object.md) for that walk in detail.

## `FreeLineHeader` — a free-list node living inside a dead line

```c
typedef struct {
    int16_t next;
    uint16_t size;
} FreeLineHeader;
```

This struct is **not** part of a line's metadata region — it's written
directly into the first bytes of an actual dead/free line's *data*, once
that line has nothing live in it to protect. This is how the in-block free
list costs zero extra memory: a hole's "node" is just whatever garbage
bytes are already sitting at the start of the hole, reinterpreted.

- **`next`** — the index of the next hole in this block, or `LAST_HOLE`
  (`-1`) if this is the last one. `BlockHeader.header.first` is the head of
  this list.
- **`size`** — how many *consecutive* lines this one hole spans (holes get
  coalesced — see `Block_Recycle` in [Block.md](../Block.md)).

The allocator's overflow-free "normal" bump path
(`Allocator_nextLineRecycled` in [Allocator.md](../Allocator.md)) is the
thing that actually walks this list: pop the current hole's `next` to
advance, multiply `size * WORDS_IN_LINE` to know how far the new `limit`
should be.
