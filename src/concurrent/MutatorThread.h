#ifndef IMMIX_MUTATOR_THREAD_H
#define IMMIX_MUTATOR_THREAD_H

#include "../GCTypes.h"
#include "../ThreadLocalAllocator.h"
#include "ValStack.h"

/**
 * One of these exists per registered mutator (OS) thread.
 */
typedef struct MutatorThread {
    // This thread's own thread-local bump allocator. Never touched by any
    // other thread, never locked.
    ThreadLocalAllocator *allocator;

    // Base address of this thread's Val stack (a Val[], not the native
    // call stack). Registered once at thread start and never moves.
    Val *stackBottom;

    // How many Val entries were live on this thread's stack the moment it
    // last hit a safepoint - recorded via ValStack_CurrentSize() by every
    // thread on itself, including the collector (it records its own right
    // before marking begins, the same way a parking thread does - there's
    // no special case for "the collector's live stack" anymore).
    // Combined with stackBottom: loop stackBottom[0 .. parkedStackSize).
    size_t parkedStackSize;

    // True only while this thread is actually blocked inside
    // MutatorSync_Poll, waiting for someone else's collection to finish.
    bool parked;
} MutatorThread;

#endif // IMMIX_MUTATOR_THREAD_H