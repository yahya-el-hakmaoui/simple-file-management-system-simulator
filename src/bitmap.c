// bitmap.c
#include <stdlib.h>
#include <string.h>
#include "bitmap.h"

Bitmap *bitmap_create(size_t size_in_bits) {
    Bitmap *bmp = malloc(sizeof(Bitmap));
    if (!bmp) return NULL;

    bmp->size_in_bits = size_in_bits;
    size_t size_in_bytes = (size_in_bits + 7) / 8;
    bmp->data = calloc(size_in_bytes, 1);

    if (!bmp->data) {
        free(bmp);
        return NULL;
    }

    return bmp;
}

void bitmap_free(Bitmap *bmp) {
    if (bmp) {
        free(bmp->data);
        free(bmp);
    }
}

void bitmap_set(Bitmap *bmp, size_t pos) {
    if (pos < bmp->size_in_bits)
        bmp->data[pos / 8] |= (1 << (pos % 8));
}

void bitmap_clear(Bitmap *bmp, size_t pos) {
    if (pos < bmp->size_in_bits)
        bmp->data[pos / 8] &= ~(1 << (pos % 8));
}

int bitmap_get(Bitmap *bmp, size_t pos) {
    if (pos < bmp->size_in_bits)
        return (bmp->data[pos / 8] >> (pos % 8)) & 1;
    return 0;
}
