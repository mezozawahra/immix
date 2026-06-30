# `concurrent/MutatorSync.h` / `MutatorSync.cpp`

The thread registry and stop-the-world coordinator. Everything in this
file is C++ (a `std::mutex`/`std::condition_variable`/`std::atomic`
implementation behind a plain `extern "C"` interface) — this is one of the
two places in the codebase (the other being
[GlobalBlockAllocator](GlobalBlockAllocator.md)/
[GlobalLargeAllocator](GlobalLargeAllocator.md)) where real synchronization
primitives are used, deliberately kept out of the C allocation fast path
entirely.

## Module state

```c
std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<MutatorThread *> g_threads;
std::atomic<bool> g_stwRequested{false};
```

One registry (`g_threads`) of every currently-registered
[`MutatorThread`](MutatorThread.md), one mutex/condvar pair guarding it
and coordinating pauses, and one atomic flag (`g_stwRequested`) that's the
single source of truth for "is a collection currently happening." The
atomic is specifically what lets `MutatorSync_Poll` have a cheap fast path
(below) — every other piece of state is only ever touched under `g_mutex`.

## Registration

```c
MutatorThread *MutatorSync_RegisterThread(Val *stackBottom) {
    auto *thread = (MutatorThread *)malloc(sizeof(MutatorThread));
    thread->allocator = NULL; // set by the caller right after this returns
    thread->stackBottom = stackBottom;
    thread->parkedStackSize = 0;
    thread->parked = false;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_threads.push_back(thread);
    return thread;
}
```

Allocates and zero-initializes a `MutatorThread`, registers it, and
returns the handle. Note `allocator` is left `NULL` here — it's the
caller's job ([`ImmixGC_RegisterThread`](../ImmixGC.md)) to immediately
create a [`ThreadLocalAllocator`](../ThreadLocalAllocator.md) for it and
assign it, since that creation itself needs to pull blocks from
`heap->globalBlockAllocator`, which `MutatorSync` has no reference to (this
file knows nothing about `Heap` at all — a deliberate layering: thread
registration and stop-the-world coordination are independent of heap
allocation policy).

`MutatorSync_UnregisterThread` is the mirror: remove from `g_threads`,
free the struct. `MutatorSync_ThreadCount` just returns `g_threads.size()`
under the lock — used by [`Heap.c`](../Heap.md) to size its
"can every thread re-prime its cursors" check.

## `MutatorSync_Poll` — the cooperative safepoint

```c
void MutatorSync_Poll(MutatorThread *self) {
    if (!g_stwRequested.load(std::memory_order_acquire)) {
        return;
    }

    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_stwRequested.load(std::memory_order_acquire)) {
        return; // resolved between the unlocked check above and this lock
    }

    self->parkedStackSize = ValStack_CurrentSize();
    self->parked = true;
    g_cv.notify_all();
    g_cv.wait(lock, [] { return !g_stwRequested.load(std::memory_order_acquire); });
    self->parked = false;
}
```

This is the function every mutator thread calls at its own allocation
entry points (`scalanative_alloc` and friends — see
[ImmixGC.md](../ImmixGC.md)) before doing any real allocation work. The
fast path — when no collection is pending, which is true the overwhelming
majority of the time — is a single atomic load, no lock at all. Only when
that load sees a pending request does the thread take the mutex, **double-check**
the flag under the lock (since the request may have already resolved
between the unlocked check and acquiring the lock — a classic
check-then-act race avoided by re-checking once synchronized), record its
own current stack size via `ValStack_CurrentSize()` (see
[ValStack.md](ValStack.md)), mark itself `parked`, wake anyone waiting on
the parked-count (the collector, see below), and then block on the
condition variable until the request clears.

The cost model this produces: a thread that never collides with a
collection pays one atomic load per allocation call and nothing else.
Only threads that happen to be allocating at the exact moment another
thread starts a collection pay the lock/park/wake cost, and only for the
duration of that one collection.

## `MutatorSync_BeginCollection` — deciding who actually collects

```c
bool MutatorSync_BeginCollection(MutatorThread *self) {
    bool expected = false;
    if (!g_stwRequested.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
        MutatorSync_Poll(self);
        return false;
    }

    self->parkedStackSize = ValStack_CurrentSize();

    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [&] {
        for (MutatorThread *t : g_threads) {
            if (t != self && !t->parked) {
                return false;
            }
        }
        return true;
    });
    return true;
}
```

Called from [`Heap_Collect`](../Heap.md) whenever a thread's allocation
fast path fails and it wants to trigger a collection. The
`compare_exchange_strong` on `g_stwRequested` is the actual race
resolution: exactly one caller will see `expected == false` and flip it to
`true`, becoming "the collector" for this round; every other caller racing
at the same instant sees the CAS fail, falls into `MutatorSync_Poll(self)`
(parking itself, exactly like a normal allocating thread would), and
returns `false` to its own caller once that wait resolves — by which point
the collection has already finished.

The winner records its own `parkedStackSize` (the same call every parking
thread makes — no special case for the collector's own roots, see
[MutatorThread.md](MutatorThread.md)) and then waits until every *other*
registered thread has `parked == true`. This is the actual stop-the-world
barrier: the collector does not proceed to
[`Marker_MarkRoots`](../Marker.md) until it can see, under the lock, that
every other mutator has reached a safepoint and is sitting parked. Threads
that are blocked on I/O, sleeping, or otherwise not calling into the
allocator at all would never reach this barrier — this design assumes
every live mutator thread allocates often enough to hit
`MutatorSync_Poll` regularly; a thread that goes a long time between
allocations would stall every collection until it allocates again.

## `MutatorSync_EndCollection`

```c
void MutatorSync_EndCollection() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stwRequested.store(false, std::memory_order_release);
    g_cv.notify_all();
}
```

Called by the collector once [`Heap_Recycle`](../Heap.md) has finished.
Clearing `g_stwRequested` and waking every waiter is what releases every
parked thread out of its `g_cv.wait` in `MutatorSync_Poll` — each one
re-checks the (now false) condition, sets `parked = false`, and returns to
its own caller to retry the allocation that originally triggered the
poll.

## `MutatorSync_ForEachThread`

```c
typedef void (*MutatorSync_ThreadVisitor)(MutatorThread *thread, void *userData);
void MutatorSync_ForEachThread(MutatorSync_ThreadVisitor visitor, void *userData);
```

A locked iteration over every registered thread, with a `void *userData`
passthrough so callers don't need a global to close over. Two current
uses: [`Marker_markValStacks`](../Marker.md) visits every thread to scan
its `Val` stack during root marking, and
[`Heap_Recycle`](../Heap.md)'s `Heap_reinitThreadCursors` visits every
thread to re-prime its [`ThreadLocalAllocator`](../ThreadLocalAllocator.md)
cursors after a collection. Both are only ever called while every other
thread is already parked (i.e. from inside the window
`MutatorSync_BeginCollection` ... `MutatorSync_EndCollection`), so the
visitor itself never needs to coordinate with the visited thread beyond
this iteration's own lock.