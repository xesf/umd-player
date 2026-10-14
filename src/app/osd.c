#include "osd.h"

#include <stdio.h>
#include <string.h>

#define SCALE      2                      /* 16-pixel text and 32-pixel icons on the 960x544 Vita screen */
#define LINE       (8 * SCALE)
#define ICON       (ICON_SIZE * SCALE)
#define PANEL_W    420
#define ROW_H      44
#define PAD        16
#define BG         RGBA(10, 14, 28, 190)
#define BG_SEL     RGBA(70, 110, 190, 230)
#define TEXT       RGBA(240, 240, 240, 255)
#define TEXT_DIM   RGBA(160, 170, 190, 255)
#define VALUE      RGBA(150, 200, 255, 255)
#define BAR_BG     RGBA(255, 255, 255, 70)
#define BAR_FG     RGBA(150, 200, 255, 255)

static uint32_t button_color(icon_id b)
{
    switch (b) {
    case ICON_BUTTON_CROSS: return COLOR_CROSS;
    case ICON_BUTTON_CIRCLE: return COLOR_CIRCLE;
    case ICON_BUTTON_TRIANGLE: return COLOR_TRIANGLE;
    case ICON_BUTTON_SQUARE: return COLOR_SQUARE;
    default: return TEXT;
    }
}

int osd_draw_hint(canvas *c, int x, int y, icon_id button, const char *text)
{
    canvas_icon(c, button, x, y, 1, button_color(button));
    return ICON_SIZE + 6 + canvas_text(c, x + ICON_SIZE + 6, y + 4, 1, TEXT_DIM, text) + 18;
}

int osd_draw_status(canvas *c, int right, int top, const char *clock, int battery, int charging)
{
    char pct[8] = "";
    int bw = 34, bh = 18, w;

    if (battery >= 0)
        snprintf(pct, sizeof(pct), "%d%%", battery);
    w = PAD + canvas_text_width(clock, SCALE) + (battery >= 0 ? 20 + bw + 8 + canvas_text_width(pct, SCALE) : 0) + PAD;
    canvas_rect(c, right - w, top, w, LINE + 24, BG);
    canvas_text(c, right - w + PAD, top + 12, SCALE, TEXT, clock);
    if (battery >= 0) {
        int x = right - w + PAD + canvas_text_width(clock, SCALE) + 20;
        canvas_battery(c, x, top + 12 + (LINE - bh) / 2, bw, bh, battery, charging, TEXT);
        canvas_text(c, x + bw + 8, top + 12, SCALE, TEXT, pct);
    }
    return w;
}

int osd_visible(const osd_view *v)
{
    return v->menu_open || v->toast[0] || v->show_bar || v->show_status;
}

static void draw_menu(const osd_view *v, canvas *c)
{
    int h = PAD + LINE + PAD + v->row_count * ROW_H + PAD + 2 * 20 + PAD;
    int x = c->w - PANEL_W - 24, y = (c->h - h) / 2 - 10, hx;

    canvas_rect(c, x, y, PANEL_W, h, BG);
    canvas_text(c, x + PAD, y + PAD, SCALE, TEXT_DIM, "OPTIONS");
    y += PAD + LINE + PAD;
    for (int i = 0; i < v->row_count; i++, y += ROW_H) {
        const osd_row *r = &v->rows[i];
        int ty = y + (ROW_H - 4 - LINE) / 2, vw = canvas_text_width(r->value, SCALE);
        int vx = x + PANEL_W - PAD - LINE - 6 - vw;
        if (i == v->menu_row)
            canvas_rect(c, x + 4, y, PANEL_W - 8, ROW_H - 4, BG_SEL);
        canvas_icon(c, r->icon, x + PAD - 4, y + (ROW_H - 4 - ICON) / 2, SCALE, i == v->menu_row ? TEXT : TEXT_DIM);
        canvas_text(c, x + PAD + ICON + 8, ty, SCALE, TEXT, r->label);
        canvas_text(c, vx, ty, SCALE, VALUE, r->value);
        if (i == v->menu_row) {
            canvas_text(c, vx - LINE - 4, ty, SCALE, VALUE, "<");
            canvas_text(c, x + PANEL_W - PAD - LINE, ty, SCALE, VALUE, ">");
        }
    }
    hx = x + PAD;
    hx += osd_draw_hint(c, hx, y + PAD, ICON_BUTTON_CROSS, "Go to chapter");
    osd_draw_hint(c, hx, y + PAD, ICON_BUTTON_CIRCLE, "Close");
    canvas_text(c, x + PAD, y + PAD + 24, 1, TEXT_DIM, "Up/Down: choose     Left/Right: change");
}

static void draw_bar(const osd_view *v, canvas *c)
{
    int h = PAD + ICON + 10 + 8 + PAD, y = c->h - h, bw = c->w - 2 * 32;

    canvas_rect(c, 0, y, c->w, h, BG);
    canvas_icon(c, v->paused ? ICON_PAUSE : ICON_PLAY, 28, y + PAD, SCALE, TEXT);
    canvas_text(c, 28 + ICON + 12, y + PAD + (ICON - LINE) / 2, SCALE, TEXT, v->title);
    canvas_text(c, c->w - 32 - canvas_text_width(v->time_text, SCALE), y + PAD + (ICON - LINE) / 2, SCALE, TEXT,
                v->time_text);
    canvas_rect(c, 32, y + PAD + ICON + 10, bw, 8, BAR_BG);
    canvas_rect(c, 32, y + PAD + ICON + 10, (int)(bw * (v->progress < 0 ? 0 : v->progress > 1 ? 1 : v->progress)), 8, BAR_FG);
}

void osd_draw(const osd_view *v, canvas *c)
{
    canvas_clear(c);
    if (v->toast[0]) {
        int w = (v->toast_icon != ICON_NONE ? ICON + 12 : 0) + canvas_text_width(v->toast, SCALE) + 2 * PAD;
        canvas_rect(c, 24, 24, w, ICON + 16, BG);
        if (v->toast_icon != ICON_NONE)
            canvas_icon(c, v->toast_icon, 24 + PAD, 24 + 8, SCALE, TEXT);
        canvas_text(c, 24 + PAD + (v->toast_icon != ICON_NONE ? ICON + 12 : 0), 24 + 8 + (ICON - LINE) / 2, SCALE, TEXT,
                    v->toast);
    }
    if (v->show_status)
        osd_draw_status(c, c->w - 24, 24, v->clock, v->battery, v->charging);
    if (v->menu_open)
        draw_menu(v, c);
    if (v->show_bar)
        draw_bar(v, c);
}
