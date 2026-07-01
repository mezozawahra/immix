#include "State.h"

Heap *heap = NULL;
Stack *stack = NULL;

// For stackoverflow handling
bool overflow = false;
uintptr_t *currentOverflowAddress = NULL;

_Thread_local MutatorThread *currentMutatorThread = NULL;