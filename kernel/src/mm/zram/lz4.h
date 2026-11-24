#ifndef AVORY_ZRAM_LZ4_H
#define AVORY_ZRAM_LZ4_H
#include <stddef.h>
/* Independent LZ4 blocks, no framing or dictionary. Zero means no fit/error. */
size_t zram_lz4_compress(const void *src, size_t length, void *dst,
                         size_t capacity);
int zram_lz4_decompress(const void *src, size_t length, void *dst,
                        size_t expected);
/* Like zram_lz4_decompress, but accepts any decompressed length <= capacity
 * and stores the produced byte count in *out_len (if non-NULL). */
int zram_lz4_decompress_partial(const void *src, size_t length, void *dst,
                                size_t capacity, size_t *out_len);
#endif
