#ifndef IMMIX_BLOCKHEADER_H
#define IMMIX_BLOCKHEADER_H

#include <stdint.h>
#include "LineHeader.h"
#include "../GCTypes.h"
#include "../Constants.h"
#include "../Log.h"

typedef enum {
    block_free        = 0x0,
    block_recyclable  = 0x1,
    block_unavailable = 0x2
} BlockFlag;

typedef struct {
    struct {
        uint8_t  mark;
        uint8_t  flags;
        int16_t  first;
        int32_t  nextBlock;
    } header;
    LineHeader lineHeaders[LINE_COUNT];
} BlockHeader;

static inline bool Block_IsRecyclable(BlockHeader *b)  { return b->header.flags == block_recyclable; }
static inline bool Block_IsUnavailable(BlockHeader *b) { return b->header.flags == block_unavailable; }
static inline bool Block_IsFree(BlockHeader *b)        { return b->header.flags == block_free; }
static inline void Block_SetFlag(BlockHeader *b, BlockFlag f) { b->header.flags = f; }
static inline bool Block_IsMarked(BlockHeader *b)  { return b->header.mark == 1; }
static inline void Block_Unmark(BlockHeader *b)    { b->header.mark = 0; }
static inline void Block_Mark(BlockHeader *b)      { b->header.mark = 1; }

/**
 * Any address inside a block -> that block's BlockHeader*.
 * Works because every block is mapped at BLOCK_TOTAL_SIZE alignment.
 * Takes void* because the caller never intends for us to dereference `addr`
 * as a typed value - we only use its numeric value for the mask.
 */
static inline BlockHeader *Block_GetBlockHeader(void *addr) {
    return (BlockHeader *)((uintptr_t)addr & BLOCK_SIZE_IN_BYTES_INVERSE_MASK);
}

/**
 * Returns the start address of line `lineIndex` as a void*.
 * Callers always cast to the type they need (Object*, FreeLineHeader*, etc.)
 * so void* is the honest return type.
 */
static inline void *Block_GetLineAddress(BlockHeader *blockHeader, int lineIndex) {
    assert(lineIndex < LINE_COUNT);
    return (ubyte_t *)blockHeader + BLOCK_METADATA_ALIGNED_SIZE
           + (lineIndex * LINE_SIZE);
}

/**
 * Returns a pointer to slot `wordIndex` within line `lineIndex`.
 * "Slot" here means an OBJ_ALIGN-byte aligned position, not a machine word.
 * Returns void* for the same reason as Block_GetLineAddress.
 */
static inline void *Block_GetLineSlot(BlockHeader *blockHeader, int lineIndex,
                                      int slotIndex) {
    assert(slotIndex < SLOTS_IN_LINE);
    return (ubyte_t *)Block_GetLineAddress(blockHeader, lineIndex)
           + (slotIndex * OBJ_ALIGN);
}

static inline FreeLineHeader *Block_GetFreeLineHeader(BlockHeader *blockHeader,
                                                      int lineIndex) {
    return (FreeLineHeader *)Block_GetLineAddress(blockHeader, lineIndex);
}

static inline BlockHeader *Block_BlockHeaderFromLineHeader(LineHeader *lineHeader) {
    return Block_GetBlockHeader(lineHeader);
}

/** Start of the usable data region (past metadata). Returns void*. */
static inline void *Block_GetDataStart(BlockHeader *blockHeader) {
    return (ubyte_t *)blockHeader + BLOCK_METADATA_ALIGNED_SIZE;
}

/** One-past-end of the usable data region. Returns void*. */
static inline void *Block_GetDataEnd(BlockHeader *blockHeader) {
    return (ubyte_t *)Block_GetDataStart(blockHeader) + (LINE_COUNT * LINE_SIZE);
}

static inline uint32_t Block_GetLineIndexFromLineHeader(BlockHeader *blockHeader,
                                                        LineHeader *lineHeader) {
    return (uint32_t)(lineHeader - blockHeader->lineHeaders);
}

/**
 * Which line does `addr` fall in?
 * addr is void* - we only need its numeric distance from the data start.
 */
static inline uint32_t Block_GetLineIndexFromWord(BlockHeader *blockHeader,
                                                  void *addr) {
    uintptr_t dataStart = (uintptr_t)Block_GetDataStart(blockHeader);
    return (uint32_t)((uintptr_t)addr - dataStart) >> LINE_SIZE_BITS;
}

static inline LineHeader *Block_GetLineHeader(BlockHeader *blockHeader,
                                              int lineIndex) {
    return &blockHeader->lineHeaders[lineIndex];
}

#endif // IMMIX_BLOCKHEADER_H