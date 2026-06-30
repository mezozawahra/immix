#ifndef IMMIX_MUTATOR_SYNC_H
#define IMMIX_MUTATOR_SYNC_H

#include "MutatorThread.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called once per OS thread, before it allocates anything. `stackBottom`
 * is the highest address of that thread's native stack - the same thing
 * `__stack_bottom` used to be for the (implicit, single) main thread.
 * Returns a handle the thread keeps for the rest of its life; the caller
 * (ImmixGC_RegisterThread) is responsible for setting `->allocator` on it
 * right after this returns.
 */
MutatorThread *MutatorSync_RegisterThread(Val *stackBottom);
void MutatorSync_UnregisterThread(MutatorThread *self);

uint32_t MutatorSync_ThreadCount(void);

/**
 * Cooperative safepoint check. Cheap (one atomic load) when no collection
 * is pending. Mutator code calls this at allocation entry points, before
 * doing any real allocation work - if a collection has been requested by
 * another thread, this parks the calling thread (recording its current
 * stack pointer as `parkedStackTop`) until that collection finishes.
 */
void MutatorSync_Poll(MutatorThread *self);

/**
 * Called by a thread that just failed to allocate and wants to run a
 * collection. Returns `true` if `self` won the race and must now actually
 * run the collection (Marker_MarkRoots + Heap_Recycle) before calling
 * MutatorSync_EndCollection(). Returns `false` if another thread is
 * already driving a collection - in that case `self` was parked for its
 * duration and the caller should just retry its allocation now.
 */
bool MutatorSync_BeginCollection(MutatorThread *self);
void MutatorSync_EndCollection(void);

/** Used by Marker_markProgramStack to visit every registered thread. */
typedef void (*MutatorSync_ThreadVisitor)(MutatorThread *thread, void *userData);
void MutatorSync_ForEachThread(MutatorSync_ThreadVisitor visitor, void *userData);

#ifdef __cplusplus
}
#endif

#endif // IMMIX_MUTATOR_SYNC_H