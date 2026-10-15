#ifndef JPEG_H
#define JPEG_H

#include <stdint.h>

/* Baseline JPEG (JFIF) decoding, for the few UMD menu images that are JPEG rather than PNG.
   Huffman-coded, 8-bit, one or three components, any sampling factors up to 4, restart
   markers. Progressive and arithmetic-coded files are not supported. Output is RGBA with
   opaque alpha. */

/* Reads the size. Returns 0 on success. */
int jpeg_info(const uint8_t *data, uint32_t size, int *width, int *height);

/* Decodes into rgba (width * height * 4 bytes, rows stride bytes apart). Returns 0 on success. */
int jpeg_decode(const uint8_t *data, uint32_t size, uint8_t *rgba, int stride);

#endif
