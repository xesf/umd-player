#include "osd.h"

#include <string.h>

#define SCALE      2                      /* 16-pixel text on the 960x544 Vita screen */
#define LINE       (8 * SCALE)
#define PANEL_W    400
#define ROW_H      40
#define PAD        16
#define BG         RGBA(10, 14, 28, 200)
#define BG_SEL     RGBA(70, 110, 190, 230)
#define TEXT       RGBA(240, 240, 240, 255)
#define TEXT_DIM   RGBA(150, 160, 180, 255)
#define VALUE      RGBA(150, 200, 255, 255)
#define BAR_BG     RGBA(255, 255, 255, 70)
#define BAR_FG     RGBA(150, 200, 255, 255)

int osd_visible(const osd_view *v)
{
    return v->menu_open || v->toast[0] || v->show_bar || v->status[0];
}

static void draw_menu(const osd_view *v, canvas *c)
{
    int h = PAD + LINE + PAD + v->row_count * ROW_H + PAD + 2 * 12 + PAD;
    int x = c->w - PANEL_W - 24, y = (c->h - h) / 2 - 20;

    canvas_rect(c, x, y, PANEL_W, h, BG);
    canvas_text(c, x + PAD, y + PAD, SCALE, TEXT_DIM, "OPTIONS");
    y += PAD + LINE + PAD;
    for (int i = 0; i < v->row_count; i++, y += ROW_H) {
        const osd_row *r = &v->rows[i];
        int vw = canvas_text_width(r->value, SCALE);
        if (i == v->menu_row)
            canvas_rect(c, x + 4, y, PANEL_W - 8, ROW_H - 4, BG_SEL);
        canvas_text(c, x + PAD, y + (ROW_H - 4 - LINE) / 2, SCALE, TEXT, r->label);
        if (i == v->menu_row) {
            canvas_text(c, x + PANEL_W - PAD - vw - 2 * LINE - 8, y + (ROW_H - 4 - LINE) / 2, SCALE, VALUE, "<");
            canvas_text(c, x + PANEL_W - PAD - LINE, y + (ROW_H - 4 - LINE) / 2, SCALE, VALUE, ">");
            canvas_text(c, x + PANEL_W - PAD - vw - LINE - 4, y + (ROW_H - 4 - LINE) / 2, SCALE, VALUE, r->value);
        } else {
            canvas_text(c, x + PANEL_W - PAD - vw - LINE - 4, y + (ROW_H - 4 - LINE) / 2, SCALE, VALUE, r->value);
        }
    }
    canvas_text(c, x + PAD, y + PAD, 1, TEXT_DIM, v->hint[0]);
    canvas_text(c, x + PAD, y + PAD + 12, 1, TEXT_DIM, v->hint[1]);
}

static void draw_bar(const osd_view *v, canvas *c)
{
    int h = PAD + LINE + 12 + 8 + PAD, y = c->h - h, bw = c->w - 2 * 32;

    canvas_rect(c, 0, y, c->w, h, BG);
    canvas_text(c, 32, y + PAD, SCALE, TEXT, v->title);
    canvas_text(c, c->w - 32 - canvas_text_width(v->time_text, SCALE), y + PAD, SCALE, TEXT, v->time_text);
    canvas_rect(c, 32, y + PAD + LINE + 12, bw, 8, BAR_BG);
    canvas_rect(c, 32, y + PAD + LINE + 12, (int)(bw * (v->progress < 0 ? 0 : v->progress > 1 ? 1 : v->progress)), 8, BAR_FG);
}

void osd_draw(const osd_view *v, canvas *c)
{
    canvas_clear(c);
    if (v->toast[0]) {
        int w = canvas_text_width(v->toast, SCALE) + 2 * PAD;
        canvas_rect(c, 24, 24, w, LINE + 2 * 12, BG);
        canvas_text(c, 24 + PAD, 24 + 12, SCALE, TEXT, v->toast);
    }
    if (v->status[0]) {
        int w = canvas_text_width(v->status, SCALE) + 2 * PAD;
        canvas_rect(c, c->w - 24 - w, 24, w, LINE + 2 * 12, BG);
        canvas_text(c, c->w - 24 - w + PAD, 24 + 12, SCALE, TEXT, v->status);
    }
    if (v->menu_open)
        draw_menu(v, c);
    if (v->show_bar)
        draw_bar(v, c);
}
