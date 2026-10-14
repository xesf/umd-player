#ifndef ICONS_H
#define ICONS_H

#include <stdint.h>
#include "canvas.h"

/* 16x16 one-colour icons for the on-screen display. */
typedef enum icon_id {
    ICON_NONE = -1,
    ICON_PLAY,
    ICON_PAUSE,
    ICON_FORWARD,
    ICON_BACK,
    ICON_NEXT_CHAPTER,
    ICON_PREV_CHAPTER,
    ICON_AUDIO,
    ICON_SUBTITLES,
    ICON_SCREEN,
    ICON_CHAPTERS,
    ICON_BUTTON_CROSS,
    ICON_BUTTON_CIRCLE,
    ICON_BUTTON_TRIANGLE,
    ICON_BUTTON_SQUARE,
    ICON_COUNT
} icon_id;

#define ICON_SIZE 16

/* PlayStation button colours, for the button icons. */
#define COLOR_CROSS    RGBA(124, 178, 232, 255)
#define COLOR_CIRCLE   RGBA(255, 102, 102, 255)
#define COLOR_TRIANGLE RGBA(64, 226, 160, 255)
#define COLOR_SQUARE   RGBA(255, 105, 248, 255)

/* Draws an icon at scale (pixels per icon pixel) with a 1-pixel shadow. */
void canvas_icon(canvas *c, icon_id icon, int x, int y, int scale, uint32_t rgba);

/* A battery outline w x h with a fill for percent, red when low, and a bolt when charging. */
void canvas_battery(canvas *c, int x, int y, int w, int h, int percent, int charging, uint32_t rgba);

#endif
