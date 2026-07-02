#ifndef IMMIX_VAL_STACK_H
#define IMMIX_VAL_STACK_H

#include "../GCTypes.h"

// Forward declaration only - the real definition (kind/bits) lives in
// your own headers and is intentionally never visible to this GC repo.
typedef struct Val Val;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Implemented by you, not by this repo - it's the only side that knows
 * about MachineType/ValueBits. Return true if val->kind is Ref or
 * RefCompressed, and if so write the decoded, mutator-visible heap
 * address into *outRef. Return false for every primitive kind without
 * touching `bits` at all.
 *
 * This assumes a Ref/RefCompressed's decoded address is always the exact,
 * canonical address Heap_Alloc returned - never an interior/offset
 * pointer. If anything in your runtime ever builds a Val that points
 * partway into an object (e.g. for fast field/array-element access),
 * precise marking like this breaks for that case - flag it back to me if
 * that's possible, since the fix is different (and loses some of the
 * benefit of having precise typing at all).
 */
bool ValStack_IsReference(const Val *val, void **outRef);

/**
 * Implemented by you. Returns how many Val entries are currently live on
 * the *calling* thread's own interpreter stack, right now. Called by
 * every thread on itself, at the moment it hits a safepoint (whether it
 * ends up parking or becomes the collector) - see MutatorSync.cpp.
 */
size_t ValStack_CurrentSize(void);

#ifdef __cplusplus
}
#endif

#endif // IMMIX_VAL_STACK_H