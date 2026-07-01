#include <stdio.h>
#include "Marker.h"
#include "Object.h"
#include "Log.h"
#include "State.h"
#include "datastructures/Stack.h"
#include "headers/ObjectHeader.h"
#include "Block.h"
#include "StackoverflowHandler.h"
#include "concurrent/GlobalLargeAllocator.h"
#include "concurrent/MutatorSync.h"
#include "concurrent/ValStack.h"

extern uintptr_t *__modules;
extern int __modules_size;

#define LAST_FIELD_OFFSET -1

void Marker_Mark(Heap *heap, Stack *stack);

void Marker_markObject(Heap *heap, Stack *stack, Object *object) {
    assert(!Object_IsMarked(&object->header));
    assert(Object_Size(&object->header) != 0);
    Object_Mark(object);
    if (!overflow) {
        overflow = Stack_Push(stack, object);
    }
}

/**
 * Kept only for any *other* raw-word root source you still have (e.g.
 * native/compiled frames holding a bare Any* outside of a Val) - it is no
 * longer called from anywhere in this file by default. If nothing else
 * needs conservative resolution, this function and Object_GetObject's
 * backward/forward line search are dead weight and can be deleted; tell
 * me and I'll take them out together.
 */
void Marker_markConservative(Heap *heap, Stack *stack, uintptr_t *address) {
    assert(Heap_IsWordInHeap(heap, address));
    Object *object = NULL;
    if (Heap_IsWordInSmallHeap(heap, address)) {
        object = Object_GetObject(address);
    } else {
        object = Object_GetLargeObject(
            GlobalLargeAllocator_Underlying(heap->globalLargeAllocator),
            address);
    }

    if (object != NULL && !Object_IsMarked(&object->header)) {
        Marker_markObject(heap, stack, object);
    }
}

void Marker_Mark(Heap *heap, Stack *stack) {
    while (!Stack_IsEmpty(stack)) {
        Object *object = Stack_Pop(stack);
        ObjectHeader *objectHeader = &object->header;
        if (Object_IsObjectArray(objectHeader)) {
            size_t size =
                Object_Size(&object->header) - OBJECT_HEADER_SIZE - OBJ_ALIGN;
            size_t nbWords = size / OBJ_ALIGN;
            for (int i = 0; i < nbWords; i++) {
                uintptr_t *field = object->fields[i];
                Object *fieldObject = Object_FromMutatorAddress(field);
                if (heap_isObjectInHeap(heap, fieldObject) &&
                    !Object_IsMarked(&fieldObject->header)) {
                    Marker_markObject(heap, stack, fieldObject);
                }
            }
        } else {
            int64_t *ptr_map = object->rtti->refMapStruct;
            int i = 0;
            while (ptr_map[i] != LAST_FIELD_OFFSET) {
                uintptr_t *field = object->fields[ptr_map[i]];
                Object *fieldObject = Object_FromMutatorAddress(field);
                if (heap_isObjectInHeap(heap, fieldObject) &&
                    !Object_IsMarked(&fieldObject->header)) {
                    Marker_markObject(heap, stack, fieldObject);
                }
                ++i;
            }
        }
    }
    StackOverflowHandler_CheckForOverflow();
}

/**
 * Precise scan of one thread's Val stack - `size` entries starting at
 * `stackBottom`. Replaces the old conservative word-by-word scan: every
 * slot here carries its own `kind` via ValStack_IsReference, so a
 * reference is resolved with one pointer subtraction
 * (Object_FromMutatorAddress), never a backward/forward search through
 * line metadata, and every primitive slot is skipped outright without
 * even a heap-range check.
 */
static void Marker_markValStack(Heap *heap, Stack *stack, Val *stackBottom,
                                size_t size) {
    for (size_t i = 0; i < size; i++) {
        uintptr_t *ref;
        if (ValStack_IsReference(&stackBottom[i], &ref) &&
            Heap_IsWordInHeap(heap, ref)) {
            Object *object = Object_FromMutatorAddress(ref);
            if (!Object_IsMarked(&object->header)) {
                Marker_markObject(heap, stack, object);
            }
        }
    }
}

typedef struct {
    Heap *heap;
    Stack *stack;
} MarkValStackContext;

static void Marker_visitThreadValStack(MutatorThread *thread, void *userData) {
    MarkValStackContext *ctx = (MarkValStackContext *)userData;
    Marker_markValStack(ctx->heap, ctx->stack, thread->stackBottom,
                        thread->parkedStackSize);
}

/**
 * Replaces Marker_markProgramStack. Visits every registered thread -
 * including the collector itself, which recorded its own parkedStackSize
 * via MutatorSync_BeginCollection right before this runs, so there's no
 * special case here for "the collector's live stack" the way the old
 * setjmp/&dummy trick needed. Nothing here is conservative anymore, so
 * there's also no jmp_buf/register-flush step left to do - that trick
 * only ever existed to force register-resident pointers onto the stack
 * for conservative scanning, and a Val written by your interpreter is
 * already sitting in memory, not transiently in a register.
 */
void Marker_markValStacks(Heap *heap, Stack *stack) {
    MarkValStackContext ctx = {heap, stack};
    MutatorSync_ForEachThread(Marker_visitThreadValStack, &ctx);
}

void Marker_markModules(Heap *heap, Stack *stack) {
    uintptr_t **modules = &__modules;
    int nb_modules = __modules_size;

    for (int i = 0; i < nb_modules; i++) {
        Object *object = Object_FromMutatorAddress(modules[i]);
        if (heap_isObjectInHeap(heap, object) &&
            !Object_IsMarked(&object->header)) {
            Marker_markObject(heap, stack, object);
        }
    }
}

void Marker_MarkRoots(Heap *heap, Stack *stack) {

    Marker_markValStacks(heap, stack);

    Marker_markModules(heap, stack);

    Marker_Mark(heap, stack);
}