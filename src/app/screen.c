#include "screen.h"

const char *screen_mode_name(screen_mode m)
{
    static const char *names[SCREEN_MODES] = {"Fit", "Zoom", "Fill", "Stretch"};
    return m < SCREEN_MODES ? names[m] : "?";
}

/* Shows region (video pixels) on screen: fitted, or filled with the overflow cropped. */
static void place(int x, int y, int w, int h, float pixel_aspect, float screen_aspect, int fill, screen_geom *out)
{
    float aspect = (float)w * pixel_aspect / (float)h;

    out->src_x = x;
    out->src_y = y;
    out->src_w = w;
    out->src_h = h;
    if (fill) {
        out->dst = (plat_rect){0, 0, 1, 1};
        if (aspect > screen_aspect) {
            int cw = (int)(w * screen_aspect / aspect + 0.5f);
            out->src_x = x + (w - cw) / 2;
            out->src_w = cw;
        } else {
            int ch = (int)(h * aspect / screen_aspect + 0.5f);
            out->src_y = y + (h - ch) / 2;
            out->src_h = ch;
        }
    } else if (aspect > screen_aspect) {
        float hh = screen_aspect / aspect;
        out->dst = (plat_rect){0, (1 - hh) / 2, 1, hh};
    } else {
        float ww = aspect / screen_aspect;
        out->dst = (plat_rect){(1 - ww) / 2, 0, ww, 1};
    }
}

void screen_layout(screen_mode m, int video_w, int video_h, int sar_w, int sar_h, const crop_rect *active,
                   float screen_aspect, screen_geom *out)
{
    float pixel_aspect = sar_w > 0 && sar_h > 0 ? (float)sar_w / (float)sar_h : 1.0f;

    switch (m) {
    case SCREEN_ZOOM:
        if (active) {
            float aspect = (float)active->w * pixel_aspect / (float)active->h;
            float crop = aspect > screen_aspect ? 1 - screen_aspect / aspect : 1 - aspect / screen_aspect;
            place(active->x, active->y, active->w, active->h, pixel_aspect, screen_aspect, crop <= ZOOM_MAX_CROP, out);
            break;
        }
        place(0, 0, video_w, video_h, pixel_aspect, screen_aspect, 0, out);
        break;
    case SCREEN_FILL:
        place(0, 0, video_w, video_h, pixel_aspect, screen_aspect, 1, out);
        break;
    case SCREEN_STRETCH:
        out->src_x = out->src_y = 0;
        out->src_w = video_w;
        out->src_h = video_h;
        out->dst = (plat_rect){0, 0, 1, 1};
        break;
    default:
        place(0, 0, video_w, video_h, pixel_aspect, screen_aspect, 0, out);
        break;
    }
}

plat_rect screen_map(const screen_geom *g, int x, int y, int w, int h)
{
    float sx = g->dst.w / g->src_w, sy = g->dst.h / g->src_h;
    plat_rect r = {g->dst.x + (x - g->src_x) * sx, g->dst.y + (y - g->src_y) * sy, w * sx, h * sy};

    if (r.x + r.w > 1)
        r.x = 1 - r.w;
    if (r.y + r.h > 1)
        r.y = 1 - r.h;
    if (r.x < 0)
        r.x = 0;
    if (r.y < 0)
        r.y = 0;
    return r;
}
