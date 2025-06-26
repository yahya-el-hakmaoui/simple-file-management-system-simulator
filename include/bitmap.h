// bitmap.h
#ifndef BITMAP_H
#define BITMAP_H

#include <stddef.h>

typedef struct {
    size_t size_in_bits;
    unsigned char *data;
} Bitmap;

Bitmap *bitmap_create(size_t size_in_bits);
void bitmap_free(Bitmap *bmp);
void bitmap_set(Bitmap *bmp, size_t pos);
void bitmap_clear(Bitmap *bmp, size_t pos);
int bitmap_get(Bitmap *bmp, size_t pos);

#endif
