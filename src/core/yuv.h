#ifndef YUV_H
#define YUV_H

#include <stdint.h>

/* 8-bit 4:2:0 (BT.601, limited range) to 32-bit BGRA with opaque alpha, the layout of a
   macOS 32BGRA buffer, a Windows 32-bit DIB and a little-endian X11 TrueColor image. */
void yuv420_to_bgra(const uint8_t *const plane[3], const int stride[3], int width, int height,
                    uint8_t *dst, int dst_stride);

#endif
