#ifndef VITA_GFX_H
#define VITA_GFX_H

#include <stdint.h>
#include "../platform.h"

/* GPU presentation: draws the layers of a plat_present_info (video, subtitle, UI) onto a
   960x544 display buffer and shows it. image is the decoded picture's framebuffer (already
   mapped for the GPU), with the picture's top-left pixel at (pic_x, pic_y).
   sceGxmInitialize must have been called. */

int vita_gfx_init(void);

/* image may be NULL (black). Returns 0 if drawn and queued for display, negative if the GPU
   path is not usable. */
int vita_gfx_present(const uint8_t *image, int image_w, int image_h, int pic_x, int pic_y, const plat_present_info *info);

#endif
