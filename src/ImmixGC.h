#ifndef IMMIX_IMMIXGC_H
#define IMMIX_IMMIXGC_H

#include "concurrent/ValStack.h"

#ifdef __cplusplus
extern "C" {
#endif

void scalanative_init();

/**
 * Call once per OS thread, including the main thread, before that thread
 * allocates anything. `stackBottom` is that thread's own Val stack base -
 * the host's interpreter owns this array; the GC only ever reads it during
 * a stop-the-world pause.
 */
void ImmixGC_RegisterThread(Val *stackBottom);
void ImmixGC_UnregisterThread();

#ifdef __cplusplus
}
#endif

#endif // IMMIX_IMMIXGC_H