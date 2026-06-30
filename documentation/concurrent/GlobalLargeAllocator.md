# `concurrent/GlobalLargeAllocator.h` / `GlobalLargeAllocator.cpp`

The large-object counterpart to
[`GlobalBlockAllocator`](GlobalBlockAllocator.md) — a thin lock around the
**entirely unmodified** [`LargeAllocator`](../LargeAllocator.md). Nothing
about the buddy-style free-list/sweep/coalescing algorithm changed for
multithreading; this file exists purely to make the existing single
`LargeAllocator` instance safe to call from more than one thread.

```c
struct GlobalLargeAllocator {
    std::mutex mutex;
    LargeAllocator *largeAllocator;
};
```

One mutex, one pointer to a real, ordinary `LargeAllocator` (see
[LargeAllocator.md](../LargeAllocator.md) for everything about how that
buddy allocator itself works — none of it changed). As with
`GlobalBlockAllocator`, this struct is only ever defined in the `.cpp`
file; C callers only ever see the opaque typedef.

```c
GlobalLargeAllocator *GlobalLargeAllocator_Create(word_t *offset, size_t size) {
    auto *allocator = new GlobalLargeAllocator();
    allocator->largeAllocator = LargeAllocator_Create(offset, size);
    return allocator;
}
```

Just constructs the wrapper and delegates straight to the unmodified
`LargeAllocator_Create`.

```c
Object *GlobalLargeAllocator_GetBlock(GlobalLargeAllocator *allocator, size_t requestedBlockSize) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    return LargeAllocator_GetBlock(allocator->largeAllocator, requestedBlockSize);
}
```

Called from [`Heap_AllocLarge`](../Heap.md) (every large-object allocation
goes through this lock — unlike small allocation, which has a per-thread
fast path via `ThreadLocalAllocator`, there is currently no thread-local
fast path for large objects at all; every large allocation contends for
this one mutex, same as the original single-mutator design just made
explicit). Worth knowing if your workload allocates large objects
(≥8KB) frequently from many threads concurrently — that's the one place
in the allocation path where contention is structural rather than
incidental.

```c
void GlobalLargeAllocator_Sweep(GlobalLargeAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    LargeAllocator_Sweep(allocator->largeAllocator);
}
```

Called once per collection, from [`Heap_Recycle`](../Heap.md), only ever
while every mutator thread is parked (same caveat as
`GlobalBlockAllocator`'s recycle-phase functions — locked defensively, not
because of expected concurrent callers today).

```c
void GlobalLargeAllocator_Grow(GlobalLargeAllocator *allocator, word_t *chunkStart, size_t incrementBytes) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    allocator->largeAllocator->size += incrementBytes;
    Bitmap_Grow(allocator->largeAllocator->bitmap, incrementBytes);
    LargeAllocator_AddChunk(allocator->largeAllocator, (Chunk *)chunkStart, incrementBytes);
}
```

Called from `Heap_GrowLarge`. Bundles all three steps the original inline
`Heap_GrowLarge` body used to do directly against `heap->largeAllocator`
(grow the recorded size, grow the bitmap, register the new range as free
chunks via the existing power-of-two-decomposition logic in
[`LargeAllocator_AddChunk`](../LargeAllocator.md)) into one locked call,
rather than three separate ones — there's no reason to release and
reacquire the lock between these three steps, since they're only ever
meaningful together.

```c
LargeAllocator *GlobalLargeAllocator_Underlying(GlobalLargeAllocator *allocator) {
    return allocator->largeAllocator;
}
```

The one function in this file that deliberately takes **no** lock. Used
exactly once, by `Marker_markConservative` (see [Marker.md](../Marker.md))
to call `Object_GetLargeObject` directly against the raw `LargeAllocator*`.
This is safe specifically because `Marker_markConservative` (where it's
still used at all — kept only for any non-`Val` raw-word root source, per
[Marker.md](../Marker.md)'s note) only ever runs during a stop-the-world
pause, when no other thread can be concurrently calling
`GlobalLargeAllocator_GetBlock`/`_Sweep`/`_Grow` on the same instance — so
reading through to the underlying allocator without the lock can't race
with anything. If this function is ever called from outside a
collection-pause context in the future, that invariant would need
re-checking.