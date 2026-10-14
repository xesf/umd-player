#ifndef DEINT_H
#define DEINT_H

#include <stdint.h>

/* In-place vertical 1-2-1 blend of a 32-bit pixel image. Mixing each line with its
   neighbours hides the comb pattern of woven fields at the cost of some vertical detail. */
void deint_blend32(uint8_t *pixels, int width, int height, int stride);

#endif
