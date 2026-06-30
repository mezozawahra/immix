# `ImmixGC.h` / `ImmixGC.c`

The actual public C ABI a host runtime calls into — process setup, the
new per-thread registration API, and the `scalanative_alloc*` family. This
is the one file in the codebase that genuinely gained new public functions
for multithreading, rather than just having existing ones adapted.

## `scalanative_init` — unchanged in shape

```c
void scalanative_init() {
    heap = Heap_Create(INITIAL_HEAP_SIZE);
    stack = Stack_Alloc(INITIAL_STACK_SIZE);
}
```

Still exactly what it was in the single-mutator version: create the one
global [`Heap`](Heap.md) and the one global mark-worklist
[`Stack`](State.md). Notably, this function does **not** register any
thread with the GC — registration is now a fully separate step (below).
Call this exactly once, process-wide, before any thread (including
whichever thread calls `scalanative_init` itself) calls
`ImmixGC_RegisterThread` or any `scalanative_alloc*` function.

## `ImmixGC_RegisterThread` / `ImmixGC_UnregisterThread` — the new per-thread lifecycle

```c
void ImmixGC_RegisterThread(Val *stackBottom) {
    MutatorThread *thread = MutatorSync_RegisterThread(stackBottom);
    thread->allocator = ThreadLocalAllocator_Create(heap->globalBlockAllocator);
    currentMutatorThread = thread;
}

void ImmixGC_UnregisterThread() {
    MutatorSync_UnregisterThread(currentMutatorThread);
    currentMutatorThread = NULL;
}
```

This is the function the single-mutator codebase simply never needed —
there was exactly one implicit mutator (the process's main thread), so
there was nothing to register. Now, **every** thread that intends to
allocate through this GC, including the main thread, must call
`ImmixGC_RegisterThread(stackBottom)` once before its first allocation.
`stackBottom` is that thread's own `Val[]` interpreter stack base — owned
and managed entirely by your runtime, not by this GC; this repo only ever
reads through it during a collection (see
[Marker.md](Marker.md)/[ValStack.md](concurrent/ValStack.md)).

Three things happen here, in order, and the order matters:

1. `MutatorSync_RegisterThread` allocates and registers a
   [`MutatorThread`](concurrent/MutatorThread.md) (see
   [MutatorSync.md](concurrent/MutatorSync.md)) with `allocator` left
   `NULL`.
2. `ThreadLocalAllocator_Create(heap->globalBlockAllocator)` builds this
   thread's own bump allocator, pulling its first blocks from the shared
   pool — this is why `scalanative_init()` must already have run: `heap`
   (and `heap->globalBlockAllocator` specifically) must exist before this
   line can succeed.
3. `currentMutatorThread = thread` stores the handle into this thread's
   own `_Thread_local` slot (see [State.md](State.md)) — from this point
   on, every `scalanative_alloc*` call on *this* thread will find its way
   back to this exact `MutatorThread` automatically.

`ImmixGC_UnregisterThread` is the mirror — deregisters from
`MutatorSync`'s registry (so a since-exited thread is no longer counted by
`MutatorSync_ThreadCount`, no longer waited on by
`MutatorSync_BeginCollection`'s stop-the-world barrier, and no longer
visited by `MutatorSync_ForEachThread`) and clears the thread-local handle.
Neither function frees the thread's `ThreadLocalAllocator` itself or
returns its currently-held blocks to the global pool — those blocks
simply become unreachable by any live mutator from that point on, and will
only be reclaimed the next time a full collection sweeps the heap and
finds nothing marking them alive. If a thread that's about to exit is
holding partially-used blocks it cares about reclaiming promptly (rather
than waiting for the next collection), that's not handled by anything in
this codebase today.

## `scalanative_alloc` family — same external signatures, new internals

```c
void *scalanative_alloc(void *info, size_t size, int isObjectArray) {
    MutatorSync_Poll(currentMutatorThread);
    size = MathUtils_RoundToNextMultiple(size, WORD_SIZE);

    void **alloc = (void **)Heap_Alloc(heap, size, isObjectArray ? true : false,
                                       currentMutatorThread);
    *alloc = info;
    return (void *)alloc;
}
```

Externally, this function's signature is identical to the single-mutator
version — same parameters, same return type — preserving the ABI for any
existing caller. Internally, two things were added, both load-bearing:

- **`MutatorSync_Poll(currentMutatorThread)` first, before any real work.**
  This is the cooperative safepoint check (see
  [MutatorSync.md](concurrent/MutatorSync.md)) — cheap (one atomic load)
  the overwhelming majority of the time, and what lets this thread cleanly
  park itself if some *other* thread has already started a collection,
  rather than racing it. Every one of `scalanative_alloc`,
  `scalanative_alloc_small`, and `scalanative_alloc_large` polls first,
  for exactly the same reason — these are the three places a host can call
  into the allocator, and each is an independent opportunity for this
  thread to be the one another thread's collection is waiting on.
- **`currentMutatorThread` passed through to `Heap_Alloc`.** This is the
  thread-local lookup described in [State.md](State.md) — it's what lets
  `Heap_Alloc` find the right `ThreadLocalAllocator` to bump-allocate from,
  and what it hands to `Heap_Collect` if that fails.

`scalanative_alloc_small`/`scalanative_alloc_large` are the same pattern,
just calling `Heap_AllocSmall`/`Heap_AllocLarge` directly instead of going
through `Heap_Alloc`'s size-based fork — presumably for callers that
already know which heap they want, bypassing the redundant size check.
`scalanative_alloc_atomic` is unchanged: it's still just
`scalanative_alloc(info, size, false)`, inheriting the polling and
thread-local lookup for free since it delegates to the function that
already does both.

## `scalanative_collect`

```c
void scalanative_collect() { Heap_Collect(heap, currentMutatorThread); }
```

The one external entry point for forcing a collection outside the normal
out-of-memory trigger. Like the allocation functions, its only change is
passing `currentMutatorThread` through — meaning if a host calls this on
an unregistered thread, `currentMutatorThread` is `NULL` and
`Heap_Collect`/`MutatorSync_BeginCollection` will be handed a `NULL`
`self`, which nothing here guards against. Same caveat as noted in
[State.md](State.md): there's no assertion catching "this thread forgot to
register" anywhere in the current code, on this path or the allocation
paths.