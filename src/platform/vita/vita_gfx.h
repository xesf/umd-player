#ifndef VITA_GFX_H
#define VITA_GFX_H

#include <stdint.h>

/* GPU presentation: draws a sub-rectangle of a linear RGBA image (already mapped for the GPU)
   onto a 960x544 display buffer, scaled to fit at the given display aspect ratio, then shows it.
   sceGxmInitialize must have been called. */

int vita_gfx_init(void);

/* An RGBA image (straight alpha) drawn over the video, placed in source pixels. */
typedef struct vita_overlay {
    const uint8_t *rgba;
    int w, h, x, y, stride;
    uint32_t id;              /* re-uploaded only when this changes */
} vita_overlay;

/* Returns 0 if the frame was drawn and queued for display, negative if the GPU path is not usable.
   overlay may be NULL. */
int vita_gfx_present(const uint8_t *image, int image_w, int image_h, int src_x, int src_y, int src_w, int src_h,
                     float display_aspect, const vita_overlay *overlay);

#endif
