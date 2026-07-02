#include <mutex>
#include "GlobalLargeAllocator.h"
#include "../datastructures/Bitmap.h"

struct GlobalLargeAllocator {
    std::mutex mutex;
    LargeAllocator *largeAllocator;
};

extern "C" GlobalLargeAllocator *GlobalLargeAllocator_Create(void *offset,
                                                              size_t size) {
    auto *allocator = new GlobalLargeAllocator();
    allocator->largeAllocator = LargeAllocator_Create(offset, size);
    return allocator;
}

extern "C" Object *GlobalLargeAllocator_GetBlock(GlobalLargeAllocator *allocator,
                                                  size_t requestedBlockSize) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    return LargeAllocator_GetBlock(allocator->largeAllocator, requestedBlockSize);
}

extern "C" void GlobalLargeAllocator_Sweep(GlobalLargeAllocator *allocator) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    LargeAllocator_Sweep(allocator->largeAllocator);
}

extern "C" void GlobalLargeAllocator_Grow(GlobalLargeAllocator *allocator,
                                          void *chunkStart,
                                          size_t incrementBytes) {
    std::lock_guard<std::mutex> lock(allocator->mutex);
    allocator->largeAllocator->size += incrementBytes;
    Bitmap_Grow(allocator->largeAllocator->bitmap, incrementBytes);
    LargeAllocator_AddChunk(allocator->largeAllocator, (Chunk *)chunkStart,
                            incrementBytes);
}

extern "C" LargeAllocator *
GlobalLargeAllocator_Underlying(GlobalLargeAllocator *allocator) {
    return allocator->largeAllocator;
}