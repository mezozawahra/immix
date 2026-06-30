#ifndef IMMIX_BLOCK_H
#define IMMIX_BLOCK_H

#include "headers/BlockHeader.h"
#include "Heap.h"
#include "Line.h"
#include "concurrent/GlobalBlockAllocator.h"

#define LAST_HOLE -1

void Block_Recycle(GlobalBlockAllocator *globalAllocator, BlockHeader *blockHeader);
void Block_Print(BlockHeader *block);
#endif // IMMIX_BLOCK_H