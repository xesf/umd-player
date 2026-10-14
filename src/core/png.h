#ifndef PNG_H
#define PNG_H

#include <stdint.h>

/* PNG decoding for subtitles (4-bit palette with transparency) and disc art (ICON0.PNG,
   PIC1.PNG). Supports every non-interlaced 8-bit-or-less colour type plus 16-bit (truncated
   to 8 bits). Adam7 interlacing is not supported. Output is RGBA, 4 bytes per pixel. */

/* Raw DEFLATE (RFC 1951). Returns the bytes written, or -1 on bad data or a full output. */
int64_t inflate_raw(const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size);

/* Reads width and height from the IHDR chunk. Returns 0 on success. */
int png_info(const uint8_t *png, uint32_t size, int *width, int *height);

/* Decodes into rgba (width * height * 4 bytes, rows stride bytes apart). work must hold
   (width * 8 + 1) * height bytes for the inflated rows. Returns 0 on success. */
int png_decode(const uint8_t *png, uint32_t size, uint8_t *rgba, int stride, uint8_t *work, uint32_t work_size);

#endif
