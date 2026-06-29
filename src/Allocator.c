#include <stdlib.h>
#include "Allocator.h"
#include "Line.h"
#include "Block.h"
#include <stdio.h>
#include <memory.h>
#include "concurrent/MutatorSync.h"

BlockHeader *Allocator_getNextBlock(Allocator *allocator);
bool Allocator_getNextLine(Allocator *allocator);

/**
 * Allocates the per-thread Allocator and primes its cursors from the
 * shared GlobalBlockAllocator. Called once per mutator thread
 * (ImmixGC_RegisterThread) and again, implicitly via Allocator_InitCursors,
 * for every registered thread after each collection.
 */
Allocator *Allocator_Create(GlobalBlockAllocator *globalAllocator) {
    Allocator *allocator = malloc(sizeof(Allocator));
    allocator->globalAllocator = globalAllocator;

    Allocator_InitCursors(allocator);

    return allocator;
}

/**
 * The Allocator needs one free block for overflow allocation and a free or
 * recyclable block for normal allocation - now checked against the shared
 * pool, accounting for every currently-registered thread needing the same.
 */
bool Allocator_CanInitCursors(Allocator *allocator) {
    return GlobalBlockAllocator_CanInitCursors(allocator->globalAllocator,
                                               MutatorSync_ThreadCount());
}

/**
 * Used to initialise the cursors of the Allocator after collection (and,
 * for a freshly registered thread, for the very first time).
 */
void Allocator_InitCursors(Allocator *allocator) {

    // Init cursor
    allocator->block = NULL;
    allocator->cursor = NULL;
    allocator->limit = NULL;

    Allocator_getNextLine(allocator);

    // Init large (overflow) cursor - always a fresh, free block, never a
    // recycled one. See the note on GlobalBlockAllocator_GetFreeBlock.
    BlockHeader *largeHeader =
        GlobalBlockAllocator_GetFreeBlock(allocator->globalAllocator);
    assert(largeHeader != NULL);
    allocator->largeBlock = largeHeader;
    allocator->largeCursor = Block_GetFirstWord(largeHeader);
    allocator->largeLimit = Block_GetBlockEnd(largeHeader);
}

/**
 * Heuristic that tells if the heap should be grown or not.
 */
bool Allocator_ShouldGrow(Allocator *allocator) {
    return GlobalBlockAllocator_ShouldGrow(allocator->globalAllocator);
}

/**
 * Overflow allocation uses only free blocks, it is used when the bump limit of
 * the fast allocator is too small to fit
 * the block to alloc.
 */
word_t *Allocator_overflowAllocation(Allocator *allocator, size_t size) {
    word_t *start = allocator->largeCursor;
    word_t *end = (word_t *)((uint8_t *)start + size);

    if (end > allocator->largeLimit) {
        BlockHeader *block =
            GlobalBlockAllocator_GetFreeBlock(allocator->globalAllocator);
        if (block == NULL) {
            return NULL;
        }
        allocator->largeBlock = block;
        allocator->largeCursor = Block_GetFirstWord(block);
        allocator->largeLimit = Block_GetBlockEnd(block);
        return Allocator_overflowAllocation(allocator, size);
    }

    if (end == allocator->largeLimit) {
        memset(start, 0, size);
    } else {
        memset(start, 0, size + WORD_SIZE);
    }

    allocator->largeCursor = end;

    Line_Update(allocator->largeBlock, start);

    return start;
}

/**
 * Allocation fast path, uses the cursor and limit. Unchanged from the
 * single-mutator version: this is the part that must stay lock-free, since
 * every mutator thread now has its own Allocator and its own cursor/limit.
 */
INLINE word_t *Allocator_Alloc(Allocator *allocator, size_t size) {
    word_t *start = allocator->cursor;
    word_t *end = (word_t *)((uint8_t *)start + size);

    // Checks if the end of the block overlaps with the limit
    if (end > allocator->limit) {
        // If it overlaps but the block to allocate is a `medium` sized block,
        // use overflow allocation
        if (size > LINE_SIZE) {
            return Allocator_overflowAllocation(allocator, size);
        } else {
            // Otherwise try to get a new line.
            if (Allocator_getNextLine(allocator)) {
                return Allocator_Alloc(allocator, size);
            }

            return NULL;
        }
    }

    if (end == allocator->limit) {
        memset(start, 0, size);
    } else {
        memset(start, 0, size + WORD_SIZE);
    }

    allocator->cursor = end;

    Line_Update(allocator->block, start);

    return start;
}

/**
 * Updates the cursor and the limit of the Allocator to point the next line of
 * the recycled block
 */
bool Allocator_nextLineRecycled(Allocator *allocator) {
    // The cursor can point on first word of next block, thus `- WORD_SIZE`
    BlockHeader *block = Block_GetBlockHeader(allocator->cursor - WORD_SIZE);
    assert(Block_IsRecyclable(block));

    int16_t lineIndex = block->header.first;
    if (lineIndex == LAST_HOLE) {
        allocator->cursor = NULL;
        return Allocator_getNextLine(allocator);
    }

    word_t *line = Block_GetLineAddress(block, lineIndex);

    allocator->cursor = line;
    FreeLineHeader *lineHeader = (FreeLineHeader *)line;
    block->header.first = lineHeader->next;
    uint16_t size = lineHeader->size;
    allocator->limit = line + (size * WORDS_IN_LINE);

    return true;
}

/**
 * Updates the the cursor and the limit of the Allocator to point to the first
 * free line of the new block.
 */
void Allocator_firstLineNewBlock(Allocator *allocator, BlockHeader *block) {
    allocator->block = block;

    // The block can be free or recycled.
    if (Block_IsFree(block)) {
        allocator->cursor = Block_GetFirstWord(block);
        allocator->limit = Block_GetBlockEnd(block);
    } else {
        assert(Block_IsRecyclable(block));
        int16_t lineIndex = block->header.first;
        assert(lineIndex < LINE_COUNT);
        word_t *line = Block_GetLineAddress(block, lineIndex);

        allocator->cursor = line;
        FreeLineHeader *lineHeader = (FreeLineHeader *)line;
        block->header.first = lineHeader->next;
        uint16_t size = lineHeader->size;
        assert(size > 0);
        allocator->limit = line + (size * WORDS_IN_LINE);
    }
}

bool Allocator_getNextLine(Allocator *allocator) {
    // If cursor is null or the block was free, we need a new block
    if (allocator->cursor == NULL ||
        // The cursor can point on first word of next block, thus `- WORD_SIZE`
        Block_IsFree(Block_GetBlockHeader(allocator->cursor - WORD_SIZE))) {
        // request the new block.
        BlockHeader *block = Allocator_getNextBlock(allocator);
        // return false if there is no block left.
        if (block == NULL) {
            return false;
        }

        Allocator_firstLineNewBlock(allocator, block);

        return true;

    } else {
        // If we have a recycled block
        return Allocator_nextLineRecycled(allocator);
    }
}

/**
 * Returns a block, first from recycled if available, otherwise from
 * the shared free-block pool. The lookup itself (and its locking) now
 * lives entirely in GlobalBlockAllocator_GetBlock.
 */
BlockHeader *Allocator_getNextBlock(Allocator *allocator) {
    return GlobalBlockAllocator_GetBlock(allocator->globalAllocator);
}