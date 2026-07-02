#include <string.h>
#include "Bitmap.h"
#include "../Constants.h"
#include "../Log.h"
#include "../utils/MathUtils.h"

Bitmap *Bitmap_Alloc(size_t size, void *offset) {
    assert(size % BITMAP_GRANULARITY == 0);

    size_t nbBlocks = size / BITMAP_GRANULARITY;

    unsigned long nbWords = MathUtils_DivAndRoundUp(nbBlocks, BITS_PER_BITMAP_WORD);
    void *words = calloc(nbWords, sizeof(bitmap_word_t));
    Bitmap *bitmap = malloc(sizeof(Bitmap));
    bitmap->data = words;
    bitmap->size = size;
    bitmap->offset = (ubyte_t *)offset;
    return bitmap;
}

size_t addressToIndex(ubyte_t *offset, ubyte_t *addr) {
    return (addr - offset) / BITMAP_GRANULARITY;
}

void Bitmap_SetBit(Bitmap *bitmap, ubyte_t *addr) {
    assert(addr >= bitmap->offset &&
           addr < bitmap->offset + bitmap->size * MIN_BLOCK_SIZE);
    size_t index = addressToIndex(bitmap->offset, addr);
    bitmap->data[WORD_OFFSET(index)] |= (1LLU << BIT_OFFSET(index));
}

void Bitmap_ClearBit(Bitmap *bitmap, ubyte_t *addr) {
    assert(addr >= bitmap->offset &&
           addr < bitmap->offset + bitmap->size * MIN_BLOCK_SIZE);

    size_t index = addressToIndex(bitmap->offset, addr);

    bitmap->data[WORD_OFFSET(index)] &= ~(1LLU << BIT_OFFSET(index));
}

int Bitmap_GetBit(Bitmap *bitmap, ubyte_t *addr) {
    assert(addr >= bitmap->offset &&
           addr < bitmap->offset + bitmap->size * MIN_BLOCK_SIZE);

    size_t index = addressToIndex(bitmap->offset, addr);
    bitmap_word_t bit =
        bitmap->data[WORD_OFFSET(index)] & (1LLU << BIT_OFFSET(index));
    return bit != 0;
}

// increment in bytes
void Bitmap_Grow(Bitmap *bitmap, size_t increment) {
    assert(increment % BITMAP_GRANULARITY == 0);

    size_t nbBlocks = bitmap->size / BITMAP_GRANULARITY;
    size_t nbBlockIncrement = increment / BITMAP_GRANULARITY;

    size_t previousNbWords = MathUtils_DivAndRoundUp(nbBlocks, BITS_PER_BITMAP_WORD);

    size_t totalNbWords =
        MathUtils_DivAndRoundUp(nbBlocks + nbBlockIncrement, BITS_PER_BITMAP_WORD);

    bitmap->data = realloc(bitmap->data, totalNbWords * sizeof(bitmap_word_t));
    bitmap->size += increment;

    memset(bitmap->data + previousNbWords, 0,
           (totalNbWords - previousNbWords) * sizeof(bitmap_word_t));
}