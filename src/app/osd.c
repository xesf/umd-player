#include "osd.h"

#include "../platform/platform.h"

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

/* The keys of docs/desktop.md's table, for the buttons that hints name. */
static const char *key_name(icon_id b)
{
    switch (b) {
    case ICON_BUTTON_CROSS: return "Space";
    case ICON_BUTTON_CIRCLE: return "Esc";
    case ICON_BUTTON_TRIANGLE: return "W";
    case ICON_BUTTON_SQUARE: return "A";
    default: return NULL;
    }
}

static int key_cap(canvas *c, int x, int y, int scale, const char *key, uint32_t color)
{
    int w = canvas_text_width(key, scale) + 8 * scale, h = ICON_SIZE * scale;
    canvas_round_rect(c, x, y, w, h, 3 * scale, RGBA(20, 24, 36, 200));
    canvas_round_ring(c, x, y, w, h, 3 * scale, scale, color);
    canvas_text(c, x + 4 * scale, y + (h - 8 * scale) / 2, scale, TEXT, key);
    return w;
}

int osd_button_width(icon_id button, int scale)
{
    const char *key = plat_keyboard_ui() ? key_name(button) : NULL;
    return key ? canvas_text_width(key, scale) + 8 * scale : ICON_SIZE * scale;
}

int osd_button(canvas *c, int x, int y, int scale, icon_id button)
{
    const char *key = plat_keyboard_ui() ? key_name(button) : NULL;
    int w = osd_button_width(button, scale), h = ICON_SIZE * scale;

    if (!key) {
        canvas_icon(c, button, x, y, scale, button_color(button));
        return w;
    }
    (void)h;
    return key_cap(c, x, y, scale, key, button_color(button));   /* the key's name, in the button's colour */
}

int osd_key_width(const char *key)
{
    return canvas_text_width(key, 1) + 8;
}

int osd_draw_key_hint(canvas *c, int x, int y, const char *key, const char *text)
{
    int kw = key_cap(c, x, y, 1, key, TEXT_DIM);
    return kw + 6 + canvas_text(c, x + kw + 6, y + 4, 1, TEXT_DIM, text) + 18;
}

int osd_draw_hint(canvas *c, int x, int y, icon_id button, const char *text)
{
    int bw = osd_button(c, x, y, 1, button);
    return bw + 6 + canvas_text(c, x + bw + 6, y + 4, 1, TEXT_DIM, text) + 18;
}

int osd_draw_status(canvas *c, int right, int top, const char *clock, int battery, int charging)
{
    char pct[16] = "";
    int bw = 34, bh = 18, w, x;

    if (battery >= 0)
        snprintf(pct, sizeof(pct), "%d%%", battery);
    w = PAD + canvas_text_width(clock, SCALE) + (battery >= 0 ? 24 + canvas_text_width(pct, SCALE) + 10 + bw : 0) + PAD;
    canvas_rect(c, right - w, top, w, LINE + 24, BG);
    x = right - w + PAD;
    x += canvas_text(c, x, top + 12, SCALE, TEXT, clock);
    if (battery >= 0) {
        x += 24;
        x += canvas_text(c, x, top + 12, SCALE, TEXT, pct) + 10 + 3;
        canvas_battery(c, x, top + 12 + (LINE - bh) / 2, bw, bh, battery, charging, TEXT);
    }
    return w;
}

int osd_visible(const osd_view *v)
{
    return v->menu_open || v->toast[0] || v->show_bar || v->show_status;
}

void osd_draw_menu(const osd_view *v, canvas *c)
{
    int sub = v->subheader[0] != 0;
    int h = PAD + LINE + (sub ? 14 : 0) + PAD + v->row_count * ROW_H + PAD + 2 * 20 + PAD;
    int x = c->w - PANEL_W - 24, y = (c->h - h) / 2 - 10, hx;
    char head[64];

    /* The disc name, cut to the panel width. */
    snprintf(head, sizeof(head), "%s", v->header[0] ? v->header : "OPTIONS");
    if ((int)strlen(head) > (PANEL_W - 2 * PAD) / LINE)
        head[(PANEL_W - 2 * PAD) / LINE] = 0;
    canvas_rect(c, x, y, PANEL_W, h, BG);
    canvas_text(c, x + PAD, y + PAD, SCALE, TEXT, head);
    if (sub)
        canvas_text(c, x + PAD, y + PAD + LINE + 4, 1, TEXT_DIM, v->subheader);
    y += PAD + LINE + (sub ? 14 : 0) + PAD;
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
    if (v->cross_hint[0])
        hx += osd_draw_hint(c, hx, y + PAD, ICON_BUTTON_CROSS, v->cross_hint);
    osd_draw_hint(c, hx, y + PAD, ICON_BUTTON_CIRCLE, "Close");
    canvas_text(c, x + PAD, y + PAD + 24, 1, TEXT_DIM, "Up/Down: choose     Left/Right: change");
}

void osd_bar_area(int w, int h, int *top, int *x0, int *x1)
{
    *top = h - (PAD + ICON + 10 + 8 + PAD);
    *x0 = 32;
    *x1 = w - 32;
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
        osd_draw_menu(v, c);
    if (v->show_bar)
        draw_bar(v, c);
}
