#ifndef IMMIX_STATE_H
#define IMMIX_STATE_H

#include "Heap.h"
#include "concurrent/MutatorThread.h"

extern Heap *heap;

// The mark-phase worklist. Only ever touched by whichever single thread is
// currently driving a collection (MutatorSync_BeginCollection guarantees
// there is at most one at a time) - marking itself is not parallelized, so
// this can safely stay one shared Stack rather than becoming per-thread.
extern Stack *stack;

extern bool overflow;
extern word_t *currentOverflowAddress;

// Each OS thread's own handle into the GC, set once by
// ImmixGC_RegisterThread and read by every allocation entry point in
// ImmixGC.c via this thread-local, so scalanative_alloc's signature never
// has to change to carry a thread argument explicitly.
extern _Thread_local MutatorThread *currentMutatorThread;

#endif // IMMIX_STATE_H