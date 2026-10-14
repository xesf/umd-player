#include "cropdetect.h"

#include <stddef.h>

#define BLACK_MAX   40     /* colour bytes at or below this count as black */
#define SAMPLES     48     /* pixels sampled per row or column */
#define ACTIVE_MIN  3      /* non-black samples needed to call a line part of the picture */

static int dark(const uint8_t *px)
{
    /* Works for RGBA and BGRA: alpha is the 4th byte in both. */
    return px[0] <= BLACK_MAX && px[1] <= BLACK_MAX && px[2] <= BLACK_MAX;
}

static int row_active(const uint8_t *pixels, int width, int stride, int y)
{
    int lit = 0;
    for (int i = 0; i < SAMPLES; i++)
        lit += !dark(pixels + (size_t)y * stride + (size_t)(width * (2 * i + 1) / (2 * SAMPLES)) * 4);
    return lit >= ACTIVE_MIN;
}

static int col_active(const uint8_t *pixels, int stride, int x, int y0, int y1)
{
    int lit = 0;
    for (int i = 0; i < SAMPLES; i++)
        lit += !dark(pixels + (size_t)(y0 + (y1 - y0) * (2 * i + 1) / (2 * SAMPLES)) * stride + (size_t)x * 4);
    return lit >= ACTIVE_MIN;
}

int crop_detect(const uint8_t *pixels, int width, int height, int stride, crop_rect *out)
{
    int top = 0, bottom = height - 1, left = 0, right = width - 1;

    while (top < height && !row_active(pixels, width, stride, top))
        top++;
    if (top == height)
        return -1;
    while (bottom > top && !row_active(pixels, width, stride, bottom))
        bottom--;
    while (left < width && !col_active(pixels, stride, left, top, bottom + 1))
        left++;
    while (right > left && !col_active(pixels, stride, right, top, bottom + 1))
        right--;
    out->x = left;
    out->y = top;
    out->w = right - left + 1;
    out->h = bottom - top + 1;
    return 0;
}
