#ifndef IMMIX_MUTATOR_THREAD_H
#define IMMIX_MUTATOR_THREAD_H

#include "../GCTypes.h"
#include "../Allocator.h"

/**
 * One of these exists per registered mutator (OS) thread. It's a plain C
 * struct so both the C allocator/marker code and the C++ synchronization
 * code can read/write its fields directly without crossing a function-call
 * boundary for the hot fields (`allocator`).
 */
typedef struct MutatorThread {
    // This thread's own thread-local bump allocator. Never touched by any
    // other thread, never locked.
    Allocator *allocator;

    // The highest address of this thread's native stack, registered once
    // at thread start (mirrors what `__stack_bottom` used to be for the
    // single, implicit main thread).
    word_t **stackBottom;

    // A snapshot of this thread's own stack pointer, taken at the moment
    // it parked at a safepoint. Only valid while `parked` is true. The
    // collector scans from here up to `stackBottom` instead of from a live
    // register snapshot, since this thread isn't running while parked.
    word_t *parkedStackTop;

    // True while this thread is blocked inside MutatorSync_Poll, waiting
    // for the active collection to finish.
    bool parked;
} MutatorThread;

#endif // IMMIX_MUTATOR_THREAD_H