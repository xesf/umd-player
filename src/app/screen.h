#ifndef SCREEN_H
#define SCREEN_H

#include "../core/cropdetect.h"
#include "../platform/platform.h"

/* Screen modes and the geometry they produce.
   - Fit: the whole picture at its display aspect ratio, with bars where the shapes differ.
   - Zoom: the detected picture inside any black borders, filling the screen when that crops
     at most ZOOM_MAX_CROP of it, otherwise fitted.
   - Fill: the whole frame scaled to cover the screen, cropping what does not fit.
   - Stretch: the whole frame stretched to the screen, ignoring the aspect ratio. */
typedef enum screen_mode {
    SCREEN_FIT,
    SCREEN_ZOOM,
    SCREEN_FILL,
    SCREEN_STRETCH,
    SCREEN_MODES
} screen_mode;

#define ZOOM_MAX_CROP 0.10f

typedef struct screen_geom {
    int src_x, src_y, src_w, src_h;   /* video pixels shown */
    plat_rect dst;                    /* where on screen, normalized */
} screen_geom;

const char *screen_mode_name(screen_mode m);

/* active is the detected picture area, or NULL to use the whole frame. */
void screen_layout(screen_mode m, int video_w, int video_h, int sar_w, int sar_h, const crop_rect *active,
                   float screen_aspect, screen_geom *out);

/* Maps a rectangle in video pixels to the screen, moved inside the screen if it would be cut,
   so subtitles in a cropped border stay visible. */
plat_rect screen_map(const screen_geom *g, int x, int y, int w, int h);

#endif
