#ifndef SOFT_H
#define SOFT_H

/* Shared by the Windows and Linux platforms: decoded frames as BGRA images in memory
   (soft_video.c), and a compositor that draws a frame with the subtitle overlay and the UI
   into a window-sized BGRA buffer (soft_compose.c). The window code then copies that buffer
   to the screen. */

#include <stdint.h>

#include "../platform.h"

struct plat_frame {
    uint8_t *bgra;
    int width, height, stride;
    int deinterlaced;          /* the field blend has been applied in place */
};

/* Draws info's layers into out (BGRA, w x h). frame may be NULL: black behind the UI. */
void soft_compose(uint8_t *out, int w, int h, int stride, plat_frame *frame, const plat_present_info *info);

/* Maps a window position to the app's 960x544 touch coordinates. */
void soft_touch_point(int x, int y, int w, int h, int *tx, int *ty);

#endif
