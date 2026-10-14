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
void plat_video_close(plat_video *v);

/* ---- Audio: 48 kHz interleaved 16-bit stereo, pulled from the app ---- */
typedef void (*plat_audio_fill)(void *user, int16_t *out, int frames);
int  plat_audio_open(int rate, plat_audio_fill fill, void *user);
uint64_t plat_audio_played(void);       /* sample frames played since open or the last reset */
void plat_audio_pause(int paused);
void plat_audio_reset(void);            /* after a seek: drop queued audio, played count to 0 */
void plat_audio_close(void);

/* ---- Screen ----
   Draws the frame scaled to the window at the display aspect ratio (sar_w:sar_h is the
   sample aspect ratio from the SPS), with the subtitle overlay on top, or black when frame
   is NULL. deinterlace blends the two fields of a frame. The app calls it again with the
   same frame when only the overlay changed. */
typedef struct plat_present_info {
    int width, height;
    int sar_w, sar_h;
    int deinterlace;
    /* Subtitle overlay: RGBA with straight alpha, placed in video pixels (0..width, 0..height).
       overlay_rgba is NULL when there is none; overlay_id changes whenever the image changes. */
    const uint8_t *overlay_rgba;
    int overlay_w, overlay_h, overlay_x, overlay_y, overlay_stride;
    uint32_t overlay_id;
} plat_present_info;

void plat_present(plat_frame *frame, const plat_present_info *info);

/* ---- Input, as Vita buttons on every platform ---- */
enum {
    PLAT_BTN_UP = 1 << 0, PLAT_BTN_DOWN = 1 << 1, PLAT_BTN_LEFT = 1 << 2, PLAT_BTN_RIGHT = 1 << 3,
    PLAT_BTN_CROSS = 1 << 4, PLAT_BTN_CIRCLE = 1 << 5, PLAT_BTN_SQUARE = 1 << 6, PLAT_BTN_TRIANGLE = 1 << 7,
    PLAT_BTN_L = 1 << 8, PLAT_BTN_R = 1 << 9, PLAT_BTN_START = 1 << 10, PLAT_BTN_SELECT = 1 << 11
};
uint32_t plat_buttons(void);            /* buttons held now */

#endif
