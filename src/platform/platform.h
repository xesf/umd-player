#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include "../core/umd_source.h"

/* The only interface between the portable player (src/app, src/core) and an OS.
   Implementations: src/platform/macos, src/platform/vita. */

/* ---- App entry points, called by the platform ----
   The platform owns the main loop because Cocoa must run on the main thread. */
int  app_init(int argc, char **argv);   /* 0 to continue, nonzero to exit */
int  app_frame(void);                   /* once per display refresh; 0 to quit */
void app_shutdown(void);

/* ---- Files ---- */
int  plat_iso_open(const char *path, umd_source *out);
void plat_iso_close(umd_source *src);

/* ---- Logging: stdout on macOS, ux0:data/umdplayer/log.txt on the Vita ---- */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Calls fn for each entry of a directory, hidden ones left out (plat_hidden_entry); is_dir is
   1 for a folder, or a link to one. Returns 0, or -1 if it cannot be read. */
typedef void (*plat_dir_fn)(void *user, const char *name, int is_dir);
int plat_list_dir(const char *dir, plat_dir_fn fn, void *user);

/* Hidden: "." and "..", folders whose names start with a dot (.Trashes, .Spotlight-V100), and
   the "._name" files macOS leaves beside files on memory cards. A disc image named
   "...As If.iso" is not hidden. */
static inline int plat_hidden_entry(const char *name, int is_dir)
{
    return name[0] == '.' && (is_dir || name[1] == '\0' || name[1] == '_' || (name[1] == '.' && name[2] == '\0'));
}

/* Small files of app data (settings, resume points), stored under a platform folder:
   ux0:data/umdplayer on the Vita, ~/.umdplayer on macOS. Load returns the bytes read or -1. */
int plat_data_load(const char *name, char *buf, int max);
int plat_data_save(const char *name, const char *buf, int size);

/* Files beside the disc images (the info sheets in <folder>/metadata/): whether path exists, and
   writing it whole, creating the folder it is in. Return 1/0, and 0 or -1. */
int plat_file_exists(const char *path);
int plat_write_file(const char *path, const char *buf, int size);

/* ---- Threads and time ---- */
typedef struct plat_thread plat_thread;
typedef struct plat_mutex plat_mutex;
typedef struct plat_cond plat_cond;

plat_thread *plat_thread_start(void (*fn)(void *), void *user, const char *name);
void plat_thread_join(plat_thread *t);
plat_mutex *plat_mutex_create(void);
void plat_mutex_lock(plat_mutex *m);
void plat_mutex_unlock(plat_mutex *m);
void plat_mutex_destroy(plat_mutex *m);
plat_cond *plat_cond_create(void);
void plat_cond_wait(plat_cond *c, plat_mutex *m);
void plat_cond_broadcast(plat_cond *c);
void plat_cond_destroy(plat_cond *c);
uint64_t plat_time_us(void);            /* monotonic */

/* ---- H.264 decoding ----
   Access units go in in decode order (Annex B, one per call, with a 90 kHz display time).
   Frames come out in any order; the app sorts them by pts. A field-coded stream produces
   one frame per field pair. */
typedef struct plat_video plat_video;
typedef struct plat_frame plat_frame;

typedef struct plat_video_config {
    int width, height;
    int interlaced;                     /* SPS frame_mbs_only_flag == 0 and field pictures in use */
    const uint8_t *sps; uint32_t sps_size;  /* NAL units without start codes */
    const uint8_t *pps; uint32_t pps_size;
} plat_video_config;

enum {
    PLAT_AU_FIELD = 1 << 0              /* the access unit is one field of a field pair */
};
#define PLAT_PTS_HIDDEN UINT64_MAX      /* decode for reference only; the frame is returned with this pts */

plat_video *plat_video_open(const plat_video_config *cfg);
/* Returns 0 on success, negative on a decode error (the app skips that access unit).
   Field pairs arrive as two calls; the frame carries the earlier of the two pts values. */
int  plat_video_decode(plat_video *v, const uint8_t *au, uint32_t size, uint64_t pts, unsigned flags);
/* Returns 1 and a frame when one is ready, 0 otherwise. The app owns the frame until it
   calls plat_frame_release. */
int  plat_video_frame(plat_video *v, plat_frame **frame, uint64_t *pts);
void plat_frame_release(plat_frame *f);
void plat_video_flush(plat_video *v);   /* drop all state after a seek */
/* The stream has ended: the pictures the decoder still holds for reordering come out through
   plat_video_frame. A flush or a seek starts it again. */
void plat_video_drain(plat_video *v);
void plat_video_close(plat_video *v);

/* ---- Audio: 48 kHz interleaved 16-bit stereo, pulled from the app ---- */
typedef void (*plat_audio_fill)(void *user, int16_t *out, int frames);
int  plat_audio_open(int rate, plat_audio_fill fill, void *user);
uint64_t plat_audio_played(void);       /* sample frames played since open or the last reset */
void plat_audio_pause(int paused);
void plat_audio_reset(void);            /* after a seek: drop queued audio, played count to 0 */
void plat_audio_close(void);

/* ---- Screen ----
   The app decides all geometry; the platform draws up to three layers, back to front:
   the video, the subtitle overlay, and the UI. Rectangles on screen are normalized (0..1,
   origin at the top left), so the app does not depend on the window size. */
typedef struct plat_rect {
    float x, y, w, h;
} plat_rect;

/* A picture of the disc menus drawn by the GPU, where plat_draws_sprites() says it can: the
   image's u v square goes to the quad x[0..3], y[0..3] on the 960x544 screen (top left, top
   right, bottom left, bottom right), showing its part u0 v0 to u1 v1, multiplied by color. */
typedef struct plat_sprite {
    const uint8_t *rgba;                /* straight alpha, w * 4 bytes a row */
    int w, h;
    uint32_t id;                        /* the same while rgba holds the same picture */
    float x[4], y[4];
    float u0, v0, u1, v1;
    float color[4];
} plat_sprite;

/* 1 if plat_present draws ui_sprites (the Vita); 0 where the app draws the UI layer itself. */
int plat_draws_sprites(void);

typedef struct plat_present_info {
    int video_w, video_h;               /* decoded picture size */
    int src_x, src_y, src_w, src_h;     /* the part of the picture to show */
    plat_rect dst;                      /* where it goes */
    int deinterlace;                    /* blend the two fields (macOS only; the Vita uses bob) */
    /* Subtitle overlay: RGBA, straight alpha. NULL for none; id changes when the image changes. */
    const uint8_t *overlay_rgba;
    int overlay_w, overlay_h, overlay_stride;
    uint32_t overlay_id;
    plat_rect overlay_dst;
    /* UI layer covering the whole screen: RGBA, straight alpha, NULL for none. */
    const uint8_t *ui_rgba;
    int ui_w, ui_h, ui_stride;
    uint32_t ui_id;
    /* Where plat_draws_sprites(): pictures in drawing order, the first ui_sprites_below of them
       under the video picture (the library's background, around its icon video) and the rest
       over it; ui_rgba, if any, goes over them all. */
    const plat_sprite *ui_sprites;
    int ui_sprite_count, ui_sprites_below;
    /* When ui_rgba changed only in rows ui_rows_from to ui_rows_to - 1 since the version with
       id ui_prev_id: a platform that still has that version may copy just those rows. 0 and 0
       for no such promise. */
    uint32_t ui_prev_id;
    int ui_rows_from, ui_rows_to;
} plat_present_info;

/* Draws the layers, or a black screen with only the UI when frame is NULL. The app calls it
   again with the same frame when only the overlay or the UI changed. */
void plat_present(plat_frame *frame, const plat_present_info *info);

/* Screen size in pixels, for aspect ratio decisions (960x544 on the Vita). */
void plat_screen_size(int *w, int *h);

/* CPU access to a decoded frame's pixels (32-bit, alpha in the 4th byte), for border detection.
   Returns 0 with the top-left pixel of the picture; call plat_frame_unlock after. */
int plat_frame_lock(plat_frame *f, const uint8_t **pixels, int *stride);
void plat_frame_unlock(plat_frame *f);

/* ---- System ---- */
/* Called about once a second while a movie plays: keeps the screen from dimming and the console
   from going to sleep, as if the user had pressed a button. */
void plat_keep_awake(void);

/* CPU speed: full while browsing and loading, lower while a movie plays (the video decoder is
   hardware; audio decoding needs about a tenth of one core). Only the Vita changes anything. */
enum { PLAT_POWER_FULL, PLAT_POWER_PLAYBACK };
void plat_set_power(int mode);

/* The system language as an ISO 639-1 code ("en"), for the default subtitle language. */
void plat_language(char out[3]);
/* One character the 8x8 font lacks, drawn with the system's fonts: see canvas_glyph_fn in
   core/canvas.h. Returns 0 when no font has it. */
int plat_glyph(uint32_t cp, int px, uint8_t *alpha);
/* 1 when the hints on screen should name keyboard keys (Space, Esc, W, A) instead of the
   Vita's buttons: on computers, until a gamepad is used. */
int plat_keyboard_ui(void);

typedef struct plat_status {
    int year, month, day, hour, minute;   /* local time */
    int battery;                          /* percent, or -1 when there is no battery */
    int charging;
} plat_status;
void plat_get_status(plat_status *s);

/* ---- Input, as Vita buttons on every platform ---- */
enum {
    PLAT_BTN_UP = 1 << 0, PLAT_BTN_DOWN = 1 << 1, PLAT_BTN_LEFT = 1 << 2, PLAT_BTN_RIGHT = 1 << 3,
    PLAT_BTN_CROSS = 1 << 4, PLAT_BTN_CIRCLE = 1 << 5, PLAT_BTN_SQUARE = 1 << 6, PLAT_BTN_TRIANGLE = 1 << 7,
    PLAT_BTN_L = 1 << 8, PLAT_BTN_R = 1 << 9, PLAT_BTN_START = 1 << 10, PLAT_BTN_SELECT = 1 << 11,
    PLAT_BTN_HELP = 1 << 12               /* F1 on a computer: the manual */
};
uint32_t plat_buttons(void);            /* buttons held now */
/* The left stick's vertical position: -127 pushed fully up to 127 fully down, 0 at rest or
   without a stick. The library scrolls with it, faster the further it is pushed. */
int plat_stick_y(void);
/* The front touch screen: 1 while a finger is on it, with its position in 960x544 screen
   pixels. 0 on platforms without one. */
int plat_touch(int *x, int *y);
/* Mouse wheel steps since the last call: positive toward the user (down the list), negative
   away. 0 on platforms without a mouse. */
int plat_scroll(void);

#endif
