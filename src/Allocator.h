#ifndef IMMIX_ALLOCATOR_H
#define IMMIX_ALLOCATOR_H

#include "GCTypes.h"
#include <stddef.h>
#include "concurrent/GlobalBlockAllocator.h"

typedef struct {
    // The shared pool every mutator's Allocator pulls blocks from. Never
    // touched directly here - only through the GlobalBlockAllocator_*
    // functions, which take care of locking.
    GlobalBlockAllocator *globalAllocator;

    BlockHeader *block;
    word_t *cursor;
    word_t *limit;

    BlockHeader *largeBlock;
    word_t *largeCursor;
    word_t *largeLimit;
} Allocator;

Allocator *Allocator_Create(GlobalBlockAllocator *globalAllocator);
bool Allocator_CanInitCursors(Allocator *allocator);
void Allocator_InitCursors(Allocator *allocator);
word_t *Allocator_Alloc(Allocator *allocator, size_t size);

bool Allocator_ShouldGrow(Allocator *allocator);

#endif // IMMIX_ALLOCATOR_H