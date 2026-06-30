# `concurrent/MutatorThread.h`

The one new piece of per-thread state every other multithreading file
revolves around. Deliberately a plain C struct, not a C++ class — both the
C allocation/marking code and the C++ synchronization code need to read
and write its fields directly, and the hottest field (`allocator`) must
never cross a function-call boundary just to be read.

```c
typedef struct MutatorThread {
    ThreadLocalAllocator *allocator;
    Val *stackBottom;
    size_t parkedStackSize;
    bool parked;
} MutatorThread;
```

One of these is created per registered OS thread (via
`MutatorSync_RegisterThread` — see [MutatorSync.md](MutatorSync.md)) and
lives for that thread's entire lifetime, from `ImmixGC_RegisterThread`
until `ImmixGC_UnregisterThread` (see [ImmixGC.md](ImmixGC.md)).

- **`allocator`** — this thread's own
  [`ThreadLocalAllocator`](../ThreadLocalAllocator.md): its bump pointer,
  limit, and overflow cursor. This is the field every allocation entry
  point (`Heap_AllocSmall`, `Heap_AllocLarge` — see [Heap.md](../Heap.md))
  reads through `self->allocator`, and it is **never** touched by any
  thread other than its owner, and never locked. Every other field on
  `MutatorThread` exists to support cross-thread coordination; this one
  field exists specifically to need none.

- **`stackBottom`** — the base address of this thread's own `Val[]`
  interpreter stack, not the native C call stack. Set once, at
  registration, and never moves for the thread's lifetime. This is the
  multithreaded, precisely-typed replacement for what used to be a single
  global `__stack_bottom` scanned conservatively — see
  [ValStack.md](ValStack.md) for why a `Val`-typed stack doesn't need
  conservative scanning at all, and [Marker.md](../Marker.md) for how this
  field gets walked during a collection.

- **`parkedStackSize`** — how many `Val` entries were live on this
  thread's stack the moment it last hit a safepoint. Combined with
  `stackBottom`, this gives the collector the exact live range to scan:
  `stackBottom[0 .. parkedStackSize)`. Every thread writes this about
  itself, via `ValStack_CurrentSize()` (a function *you* implement — see
  [ValStack.md](ValStack.md)) — there is no cross-thread snapshotting of
  someone else's live stack pointer happening anywhere. Notably, the
  active collector thread also records its own `parkedStackSize` this same
  way, right before marking begins (`MutatorSync_BeginCollection` does
  this) — there's no separate code path for "the collector's own roots"
  the way the old single-mutator design needed a `setjmp`/register-flush
  trick to force its own roots onto its own stack for conservative
  scanning. A `Val` written by the interpreter is already sitting in
  memory the moment it's written, not transiently live only in a register,
  so there's nothing to flush.

- **`parked`** — `true` only while this thread is genuinely blocked inside
  `MutatorSync_Poll`, waiting for someone else's collection to finish. This
  is the flag `MutatorSync_BeginCollection`'s wait condition checks across
  every other registered thread before letting the collector proceed —
  see [MutatorSync.md](MutatorSync.md).

## Why this is a flat struct instead of, say, three separate global maps

The single-mutator codebase had exactly one of everything (`State.h`'s
`Heap *heap`, the old global `Stack *stack`, an implicit single
`Allocator`), so there was nothing to key by thread. With multiple
mutators, the natural alternative to one struct per thread would be
several parallel `std::unordered_map<thread_id, ...>` lookups (one for the
allocator, one for the stack range, one for parked state) — `MutatorThread`
avoids that: one allocation per thread, one pointer (held in the
thread-local `currentMutatorThread`, see [State.md](../State.md)) gives
every piece of per-thread state in a single dereference, and the registry
in [`MutatorSync`](MutatorSync.md) only ever needs to store
`MutatorThread*` handles, not separately keyed lookups.