#include "yuv.h"

/* 16.16 fixed point coefficients of the BT.601 inverse matrix with the 16..235 range */
void yuv420_to_bgra(const uint8_t *const plane[3], const int stride[3], int width, int height,
                    uint8_t *dst, int dst_stride)
{
    for (int y = 0; y < height; y++) {
        const uint8_t *py = plane[0] + y * stride[0];
        const uint8_t *pu = plane[1] + (y >> 1) * stride[1];
        const uint8_t *pv = plane[2] + (y >> 1) * stride[2];
        uint8_t *o = dst + y * dst_stride;
        for (int x = 0; x < width; x++) {
            int c = (py[x] - 16) * 76309, d = pu[x >> 1] - 128, e = pv[x >> 1] - 128;
            int r = (c + 104597 * e + 32768) >> 16;
            int g = (c - 25675 * d - 53279 * e + 32768) >> 16;
            int b = (c + 132201 * d + 32768) >> 16;
            o[x * 4 + 0] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            o[x * 4 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[x * 4 + 2] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[x * 4 + 3] = 255;
        }
    }
}
