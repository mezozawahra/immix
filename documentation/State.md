# `State.h` / `State.c`

The handful of process-wide globals everything else hangs off of. In the
single-mutator codebase this used to be the *entire* picture of shared
state (`heap`, `stack`, the overflow flags); post-multithreading, it's
deliberately still small — almost everything that used to be implicitly
"the one mutator's state" moved out into per-thread
[`MutatorThread`](concurrent/MutatorThread.md) structs instead of staying
here as more globals.

```c
extern Heap *heap;
extern Stack *stack;
extern bool overflow;
extern word_t *currentOverflowAddress;
extern _Thread_local MutatorThread *currentMutatorThread;
```

## `heap` — unchanged in kind, just points to a different struct shape now

Still exactly one global `Heap *`, created once by `scalanative_init`
(see [ImmixGC.md](ImmixGC.md)) and never reassigned afterward. What
changed is what `Heap` itself contains — see [Heap.md](Heap.md) for the
`GlobalBlockAllocator`/`GlobalLargeAllocator` split that replaced the old
single `Allocator`/`LargeAllocator` fields. `State.h` itself needed no
changes here beyond the comment.

## `stack` — the mark worklist, deliberately still one global

```c
// The mark-phase worklist. Only ever touched by whichever single thread is
// currently driving a collection (MutatorSync_BeginCollection guarantees
// there is at most one at a time) - marking itself is not parallelized, so
// this can safely stay one shared Stack rather than becoming per-thread.
extern Stack *stack;
```

This is the one piece of GC-internal state that easily *could* have been
mistaken for needing to move into `MutatorThread` (since every other
piece of formerly-global, formerly-implicit-single-mutator state did), but
deliberately didn't. The reasoning is a direct consequence of how
[`MutatorSync_BeginCollection`](concurrent/MutatorSync.md) works: it
guarantees that at most one thread is ever actively driving a collection
at a time — every other thread that wanted to collect at the same moment
is parked instead, not concurrently marking. Since `Marker_Mark` and
`Marker_markObject` (see [Marker.md](Marker.md)) are the only things that
ever push onto or pop from this `Stack`, and they only ever run on the
single winning collector thread, one shared `Stack` remains correct
without any locking of its own. If marking is ever parallelized in the
future (multiple worker threads draining the worklist concurrently), this
is the field that would need to change — either into per-worker
worklists, or a genuinely thread-safe work-stealing structure.

`overflow`/`currentOverflowAddress` are the existing
[`StackoverflowHandler`](StackoverflowHandler.md) mechanism (recovery when
this worklist fills up mid-trace) and are subject to the exact same
reasoning — both are only ever touched by the single active collector
thread, so neither needed to move or become per-thread.

## `currentMutatorThread` — the one genuinely new global, and it's thread-local

```c
// Each OS thread's own handle into the GC, set once by
// ImmixGC_RegisterThread and read by every allocation entry point in
// ImmixGC.c via this thread-local, so scalanative_alloc's signature never
// has to change to carry a thread argument explicitly.
extern _Thread_local MutatorThread *currentMutatorThread;
```

This is how every allocation entry point in [`ImmixGC.c`](ImmixGC.md)
(`scalanative_alloc`, `scalanative_alloc_small`, etc.) finds *its own
calling thread's* [`MutatorThread`](concurrent/MutatorThread.md) — and
therefore its own [`ThreadLocalAllocator`](ThreadLocalAllocator.md) —
without needing a `MutatorThread *` parameter added to their public
signatures. `_Thread_local` (C11) means this isn't really one global
variable; it's one storage slot *per OS thread*, each independently
`NULL` until that specific thread calls `ImmixGC_RegisterThread`.

This is a deliberate ABI-preservation choice: `Heap_Alloc`/
`Heap_AllocSmall`/`Heap_AllocLarge`/`Heap_Collect` all *did* need their
signatures extended with an explicit `MutatorThread *self` parameter (see
[Heap.md](Heap.md)) since they're called both from `ImmixGC.c` and
recursively from each other and need to thread that pointer through
explicitly. But `scalanative_alloc` and friends are the actual public,
externally-called entry points a host runtime invokes — changing *their*
signature to take a thread argument would mean every call site outside
this codebase would need updating too. Reading `currentMutatorThread`
internally avoids that: from the outside, `scalanative_alloc(info, size,
isObjectArray)` looks exactly as it always did.

**Implicit ordering requirement worth knowing:** nothing in `State.c`
enforces it, but `currentMutatorThread` is only valid on a thread after
that thread has called `ImmixGC_RegisterThread` — and `scalanative_init()`
(which creates `heap`) must have already run process-wide before *any*
thread calls `ImmixGC_RegisterThread`, since registration immediately
reads `heap->globalBlockAllocator` to build that thread's allocator (see
[ImmixGC.md](ImmixGC.md)). Calling any `scalanative_alloc*` function on a
thread that skipped registration will dereference a `NULL`
`currentMutatorThread` inside `Heap_Alloc`. There's no assertion guarding
this today — worth adding one if this is a misuse you want caught early
rather than as a segfault.