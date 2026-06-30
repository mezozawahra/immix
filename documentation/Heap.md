# `Heap.h` / `Heap.c`

The top-level object every other subsystem hangs off of. It owns two
**physically separate** mmap'd regions — not one heap split by a size
threshold at some address, but two independent address ranges each grown
independently — and now, post-multithreading, neither region is owned
directly by a single allocator struct anymore. Every `Heap_IsWordInSmallHeap`/
`Heap_IsWordInLargeHeap` check elsewhere in the codebase is a simple range
comparison against these two regions.

## The struct

```c
typedef struct {
    size_t memoryLimit;
    word_t *heapStart;
    word_t *heapEnd;
    size_t smallHeapSize;
    word_t *largeHeapStart;
    word_t *largeHeapEnd;
    size_t largeHeapSize;

    GlobalBlockAllocator *globalBlockAllocator;
    GlobalLargeAllocator *globalLargeAllocator;
} Heap;
```

`memoryLimit` is read once at creation via `getMemorySize()`
([Memory.md](Memory.md)) — total physical RAM — and acts as a hard ceiling
neither `heapEnd` nor `largeHeapEnd` will ever be grown past
(`Heap_isGrowingPossible` checks `smallHeapSize + largeHeapSize +
increment <= memoryLimit`; if growth would cross it, the process aborts
via `Heap_exitWithOutOfMemory`, which prints a stack trace via
[StackTrace.h](StackTrace.md) and calls `exit(1)`).

**What changed from the single-mutator design:** `Heap` used to own an
`Allocator *allocator` and a `LargeAllocator *largeAllocator` directly —
one allocator each, used by whichever single thread was running. Now every
mutator thread has its own [`ThreadLocalAllocator`](ThreadLocalAllocator.md)
(reached through its [`MutatorThread`](concurrent/MutatorThread.md), not
through `Heap` at all), and `Heap` instead owns the two **shared pools**
those thread-local allocators pull blocks/chunks from:
[`GlobalBlockAllocator`](concurrent/GlobalBlockAllocator.md) (small-heap
blocks) and [`GlobalLargeAllocator`](concurrent/GlobalLargeAllocator.md)
(large-heap chunks — a thin lock around the otherwise-unchanged
[`LargeAllocator`](LargeAllocator.md)). `Heap.c` itself never touches a
`BlockList` or a free list directly anymore; it only ever calls into these
two locked wrappers.

## `Heap_mapAndAlign` — unchanged

Still maps the entire `memoryLimit` (i.e. all of physical RAM) up front
with `MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS`, then manually
realigns forward to a block boundary if `mmap` didn't already return an
aligned address. Nothing about this needed to change for multithreading —
it runs once, before any mutator thread exists.

## `Heap_Create`

```c
heap->globalBlockAllocator = GlobalBlockAllocator_Create(
    smallHeapStart, initialSize / BLOCK_TOTAL_SIZE);
...
heap->globalLargeAllocator =
    GlobalLargeAllocator_Create(largeHeapStart, initialSize);
```

Same two-region setup as before (small heap aligned to `BLOCK_TOTAL_SIZE`,
large heap aligned to `MIN_BLOCK_SIZE`), but now seeding the two *global*
allocators instead of building `Allocator`/`LargeAllocator` structs
in-place. Notably, **no `ThreadLocalAllocator` is created here at all** —
`Heap_Create` only sets up the shared pools. Each mutator thread creates
its own `ThreadLocalAllocator` later, when it registers itself via
`ImmixGC_RegisterThread` (see [ImmixGC.md](ImmixGC.md)), pulling its first
blocks from `heap->globalBlockAllocator` at that point.

## `Heap_Alloc` — the size-based fork, now thread-aware

```c
word_t *Heap_Alloc(Heap *heap, uint32_t objectSize, bool isObjectArray,
                   MutatorThread *self) {
    if (objectSize + OBJECT_HEADER_SIZE >= LARGE_BLOCK_SIZE) {
        return Heap_AllocLarge(heap, objectSize, isObjectArray, self);
    } else {
        return Heap_AllocSmall(heap, objectSize, isObjectArray, self);
    }
}
```

The size-based routing logic is identical to before — purely a comparison
against `LARGE_BLOCK_SIZE` (8KB), nothing about an object's type matters.
The only change is the new `MutatorThread *self` parameter, threaded all
the way through every allocation entry point now. It serves two purposes:
it's where `Heap_AllocSmall` finds the calling thread's own
`ThreadLocalAllocator` (`self->allocator`) for the bump-pointer fast path,
and it's what gets handed to `Heap_Collect` if that fast path fails, so
the collector knows which thread is asking and can register/park it
correctly via `MutatorSync`.

## `Heap_AllocSmall` / `Heap_allocSmallSlow` — same retry ladder, per-thread allocator

```c
INLINE word_t *Heap_AllocSmall(Heap *heap, uint32_t objectSize,
                               bool isObjectArray, MutatorThread *self) {
    uint32_t size = objectSize + OBJECT_HEADER_SIZE;
    Object *object = (Object *)ThreadLocalAllocator_Alloc(self->allocator, size);
    if (object != NULL) {
        /* stamp metadata, return */
    } else {
        return Heap_allocSmallSlow(heap, size, isObjectArray, self);
    }
}
```

Structurally unchanged from the single-mutator version: try the fast path
once on `self`'s own allocator (no lock — the bump pointer is exclusively
owned by this thread), and on failure fall to the slow path:

```c
word_t *Heap_allocSmallSlow(Heap *heap, uint32_t size, bool isObjectArray,
                            MutatorThread *self) {
    Heap_Collect(heap, self);
    Object *object = (Object *)ThreadLocalAllocator_Alloc(self->allocator, size);
    if (object == NULL) {
        Heap_Grow(heap, size);
        object = (Object *)ThreadLocalAllocator_Alloc(self->allocator, size);
        assert(object != NULL);
    }
    /* stamp metadata, return */
}
```

Same three-step ladder as before — collect, retry; grow, retry; assert if
that still fails — but `Heap_Collect(heap, self)` now does real work
beyond what it used to: see below. As before, that final `assert` compiles
away under `NDEBUG` (the default — see [Log.md](Log.md)), so a genuine
third-attempt failure still silently returns a garbage pointer rather than
crashing loudly; this is an existing gap in the original design, not
something multithreading introduced or fixed.

## `Heap_AllocLarge` — same ladder, and a fixed bug

```c
word_t *Heap_AllocLarge(Heap *heap, uint32_t objectSize, bool isObjectArray,
                        MutatorThread *self) {
    Object *object = GlobalLargeAllocator_GetBlock(heap->globalLargeAllocator, size);
    if (object == NULL) {
        Heap_Collect(heap, self);
        object = GlobalLargeAllocator_GetBlock(heap->globalLargeAllocator, size);
        if (object == NULL) {
            Heap_GrowLarge(heap, size);
            object = GlobalLargeAllocator_GetBlock(heap->globalLargeAllocator, size);
            assert(object != NULL);
        }
    }
    ObjectHeader *objectHeader = &object->header;
    Object_SetObjectType(objectHeader, object_large);
    Object_SetObjectArray(objectHeader, isObjectArray);
    Object_SetSize(objectHeader, size);
    return Object_ToMutatorAddress(object);
}
```

Same try → collect-and-retry → grow-and-retry ladder, now routed through
the locked `GlobalLargeAllocator_GetBlock` instead of calling
`LargeAllocator_GetBlock` directly. Worth flagging: this version also
fixes a latent bug present in the original single-mutator code, where the
middle branch (the retry immediately after the first collection) forgot
to call `Object_SetObjectArray` — meaning an object array that happened to
satisfy allocation on exactly that retry would silently keep whatever
garbage `isObjectArray` byte was already sitting in that memory, rather
than the caller's actual flag. All three paths here call
`Object_SetObjectArray` consistently.

## `Heap_Collect` — now a stop-the-world race, not a guaranteed run

```c
void Heap_Collect(Heap *heap, MutatorThread *self) {
    if (!MutatorSync_BeginCollection(self)) {
        return;
    }
    Marker_MarkRoots(heap, stack);
    Heap_Recycle(heap);
    MutatorSync_EndCollection();
}
```

This is the one function in `Heap.c` whose *shape* genuinely changed, not
just its parameter list. In the single-mutator version, calling
`Heap_Collect` always meant "trace and recycle, right now, on this
thread." With multiple mutators, two or more threads can run out of
allocation space at roughly the same moment — only one of them should
actually drive a collection while the others wait for it to finish, not
race each other through `Marker_MarkRoots`/`Heap_Recycle` concurrently.

`MutatorSync_BeginCollection(self)` (see
[MutatorSync.md](concurrent/MutatorSync.md)) is the arbiter: it returns
`true` to exactly one caller — the thread that "won" — and only that
thread proceeds to actually trace and recycle. Every other thread that
calls `Heap_Collect` around the same time gets `false` back, having
already been parked by `MutatorSync_BeginCollection` for the full duration
of the winner's collection; by the time `false` is returned to them, the
collection has *already happened*, so they simply return immediately and
let their own caller (`Heap_allocSmallSlow` / `Heap_AllocLarge`) retry the
allocation now that the heap has fresh space. There is no special case
written anywhere for "I lost the race" beyond this early return — the
retry-after-collect logic in both allocation paths doesn't know or care
whether this thread was the collector or just a parked bystander.

Note `Marker_MarkRoots(heap, stack)` still uses the single global `stack`
(the mark worklist, declared in [State.md](State.md)) — marking itself is
**not** parallelized. Only one thread is ever inside `Marker_MarkRoots` at
a time, by construction (`MutatorSync_BeginCollection` guarantees at most
one winner), so a single shared worklist remains correct without its own
locking.

## `Heap_Recycle` — now sweeps through the global allocators, and re-primes every thread

```c
void Heap_Recycle(Heap *heap) {
    GlobalBlockAllocator_BeginRecycle(heap->globalBlockAllocator);

    word_t *current = heap->heapStart;
    while (current != heap->heapEnd) {
        Block_Recycle(heap->globalBlockAllocator, (BlockHeader *)current);
        current += WORDS_IN_BLOCK;
    }
    GlobalLargeAllocator_Sweep(heap->globalLargeAllocator);

    uint32_t threadCount = MutatorSync_ThreadCount();
    if (!GlobalBlockAllocator_CanInitCursors(heap->globalBlockAllocator, threadCount) ||
        GlobalBlockAllocator_ShouldGrow(heap->globalBlockAllocator)) {
        size_t increment = heap->smallHeapSize / WORD_SIZE * GROWTH_RATE / 100;
        increment = (increment - 1 + WORDS_IN_BLOCK) / WORDS_IN_BLOCK * WORDS_IN_BLOCK;
        Heap_Grow(heap, increment);
    }

    MutatorSync_ForEachThread(Heap_reinitThreadCursors, NULL);
}
```

Same overall shape as the single-mutator sweep — `BeginRecycle` resets the
shared bookkeeping (the analog of the old direct `BlockList_Clear` calls,
now behind a lock in [`GlobalBlockAllocator`](concurrent/GlobalBlockAllocator.md)),
then a linear walk over every block address in the small heap calls
`Block_Recycle` exactly as before (see [Block.md](Block.md) for what that
does per-block), then the large heap gets `GlobalLargeAllocator_Sweep`
(an unchanged `LargeAllocator_Sweep` underneath, just locked).

Two things are genuinely different here versus the single-mutator
version, both consequences of there now being more than one allocator to
satisfy:

- **The growth check now accounts for every registered thread, not just
  one.** `GlobalBlockAllocator_CanInitCursors`/`_ShouldGrow` take
  `MutatorSync_ThreadCount()` — every currently-registered mutator will
  need to re-prime *both* its normal and overflow cursor after this
  recycle, so "is there enough free/recycled space to init cursors" now
  means "enough for everyone," not "enough for the one `Allocator` the
  heap used to own." See
  [GlobalBlockAllocator.md](concurrent/GlobalBlockAllocator.md) for the
  exact arithmetic.
- **Re-priming happens for every thread, not implicitly for "the"
  allocator.** The old code ended with a single `Allocator_InitCursors(heap->allocator)`
  call. Now there's no one allocator to re-prime — `MutatorSync_ForEachThread`
  visits every registered `MutatorThread` and calls
  `ThreadLocalAllocator_InitCursors` on each one's own allocator via the
  small `Heap_reinitThreadCursors` visitor function. This is correct *only*
  because `Heap_Recycle` is only ever reached from `Heap_Collect` while
  every other mutator is parked (guaranteed by `MutatorSync_BeginCollection`)
  — re-priming a thread's cursors while it might still be mid-allocation
  on them would be a race.

## `Heap_Grow` / `Heap_GrowLarge`

Same "just extend `heapEnd`/`largeHeapEnd` further into the already-mapped
region, no new `mmap` call" strategy as before. The only change is what
they report the new space to: `Heap_Grow` calls
`GlobalBlockAllocator_AddFreeBlocks` (an O(1) bulk-append of the new,
already block-chained range, locked) instead of calling
`BlockList_AddBlocksLast` on `heap->allocator->freeBlocks` directly.
Likewise, `Heap_Grow`'s own can't-grow-further check now calls
`GlobalBlockAllocator_CanInitCursors(heap->globalBlockAllocator, MutatorSync_ThreadCount())`
— the same per-thread-aware check `Heap_Recycle` uses, rather than the old
single-allocator version. `Heap_GrowLarge` calls `GlobalLargeAllocator_Grow`,
which itself grows the underlying `LargeAllocator`'s size, its
`Bitmap`, and registers the new range as free chunks, all under one lock
(see [GlobalLargeAllocator.md](concurrent/GlobalLargeAllocator.md)).