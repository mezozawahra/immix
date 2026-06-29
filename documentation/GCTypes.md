# `GCTypes.h`

The smallest file in the codebase, but two of its three definitions show up
in almost every other file.

```c
typedef uintptr_t word_t;
typedef uint8_t ubyte_t;

#define INLINE __attribute__((always_inline))
```

- **`word_t`** — every pointer-sized quantity in the GC is typed as
  `word_t` rather than `void*` or a concrete pointer type. Sizes, offsets,
  and addresses being treated as raw "machine words" rather than typed
  pointers is what lets code like `Block_GetBlockHeader`'s pointer-masking
  trick (see [Constants.md](Constants.md)) do bitwise arithmetic on an
  address without a cast dance. It also makes the GC's word size track
  whatever `uintptr_t` is on the target platform (4 bytes on 32-bit, 8 on
  64-bit) rather than hard-coding 8.
- **`ubyte_t`** — `uint8_t`, used wherever code wants to do byte-granular
  pointer arithmetic (`(ubyte_t *)x + offset` advances by exactly `offset`
  *bytes*, whereas `(word_t *)x + offset` would advance by `offset` words).
  Both styles show up side by side in this codebase depending on whether
  the offset at hand is byte- or word-denominated.
- **`INLINE`** — a force-inline attribute (stronger than the `inline`
  keyword's hint — GCC/Clang will inline the function even at `-O0` or when
  their normal heuristics would refuse). Applied to the handful of
  functions that sit on the hot allocation path and are small enough that a
  call/return would be a meaningful fraction of their total cost:
  `Allocator_Alloc`, `Heap_AllocSmall`, and the line-update helpers in
  `Line.h`.
