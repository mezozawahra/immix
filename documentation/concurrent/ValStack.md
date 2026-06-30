# `concurrent/ValStack.h`

The seam between this GC and your interpreter's actual `Val`/`MachineType`
representation. This repo intentionally knows nothing about the real
layout of `Val` — it only ever sees a forward declaration — so this file
declares two functions that *you* implement on your side, and the GC calls
through them rather than ever inspecting a `Val`'s bytes itself.

```c
typedef struct Val Val;   // forward declaration only

bool ValStack_IsReference(const Val *val, word_t **outRef);
size_t ValStack_CurrentSize(void);
```

## `ValStack_IsReference` — decode one stack slot

Given one `Val`, this is expected to return `true` and write the decoded,
mutator-visible heap address into `*outRef` if `val->kind` is `Ref` or
`RefCompressed`; otherwise return `false` without touching `bits` at all.
This is what lets [`Marker_markValStack`](../Marker.md) treat every
primitive `MachineType` (`Int32`, `Bool`, `Double`, ...) as a guaranteed
skip — no heap-range check, no resolution attempt — while every reference
slot is resolved with a single pointer subtraction
(`Object_FromMutatorAddress`, see
[headers/ObjectHeader.md](../headers/ObjectHeader.md)) instead of the
old conservative backward/forward line search
(`Object_GetObject`/`Marker_markConservative`).

Two preconditions this function's contract depends on, both worth
re-checking on your side before relying on it in production:

- **Decoded addresses must always be canonical, never interior.** The
  contract assumes a `Ref`/`RefCompressed`'s decoded address is always
  exactly what `Heap_Alloc` returned for that object — never a pointer
  partway into the object's fields (e.g. for fast field/array-element
  access). If your runtime ever needs interior pointers, precise marking
  as implemented here breaks for that case, since `Object_FromMutatorAddress`
  assumes a fixed one-word offset back to the header.
- **`RefCompressed` decode assumes heap base `0`.** Per its own
  documentation, the encoding is `real_address >> 3`, decoded as
  `ptr32 << 3` — which only round-trips correctly if the real heap base is
  `0`. `Heap_mapAndAlign` (see [Heap.md](../Heap.md)) calls `mmap(NULL,
  ...)`, so the OS picks the actual base address — it will not be `0` in
  practice. If `RefCompressed` is actually in use (rather than reserved
  for later), this needs the heap's real base folded back in somewhere,
  or it will decode to the wrong address.

## `ValStack_CurrentSize` — how much of this thread's stack is live, right now

Returns how many `Val` entries are currently live on the *calling*
thread's own stack. Called by every thread on itself, never on another
thread — once when it parks at a safepoint
(`MutatorSync_Poll`), and once by whichever thread wins the race to become
the collector (`MutatorSync_BeginCollection`), both in
[MutatorSync.cpp](MutatorSync.md). The result is stored into that thread's
own `MutatorThread.parkedStackSize` (see
[MutatorThread.md](MutatorThread.md)).

## Why two functions instead of one combined "give me the roots" callback

Splitting "is this slot a reference" from "how big is the stack right now"
keeps the seam minimal and keeps the scanning loop itself
(`Marker_markValStack` in [Marker.md](../Marker.md)) inside this repo,
where it can be written once against `Heap_IsWordInHeap`/
`Object_FromMutatorAddress` rather than re-implemented per-host. The only
things genuinely specific to your `Val` representation — decoding a
union by `kind`, and knowing your own stack's current depth — are exactly
the two things pushed out to you.