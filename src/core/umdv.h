#ifndef UMDV_H
#define UMDV_H

#include <stdint.h>

/* UMD Video navigation files: UMD_VIDEO/PLAYLIST.UMD and the .CLP files in UMD_VIDEO/CLIPINF.
   Layouts are in docs/formats/umd-video.md. All times are 90 kHz ticks; streams start at 90000. */

#define UMDV_PTS_HZ 90000u

#define UMDV_MAX_TITLES   256
#define UMDV_MAX_CHAPTERS 2048
#define UMDV_MAX_NAME     128
#define UMDV_MAX_STREAMS  48

typedef struct umdv_chapter {
    uint32_t pts;
    char name[25];            /* "Mark1783": UMD-PG scripts read it as mark_name */
    uint32_t data;            /* bytes 0x0C-0x0F of the entry, mark_data (0 on the test discs) */
} umdv_chapter;

typedef struct umdv_title {
    char name[UMDV_MAX_NAME];
    char clip[9];            /* "00001": selects CLIPINF/<clip>.CLP and STREAM/<clip>.MPS */
    uint32_t start_pts;
    uint32_t end_pts;
    uint16_t first_chapter;  /* index into umdv_playlist.chapters */
    uint16_t chapter_count;
} umdv_title;

typedef struct umdv_playlist {
    char name[256];          /* authoring name, not the movie title; use PARAM.SFO TITLE for that */
    uint16_t title_count;
    uint16_t chapter_count;
    umdv_title titles[UMDV_MAX_TITLES];
    umdv_chapter chapters[UMDV_MAX_CHAPTERS];
} umdv_playlist;

/* Returns 0 on success, negative on a malformed file. */
int umdv_playlist_parse(const uint8_t *buf, uint32_t size, umdv_playlist *out);

typedef enum umdv_stream_kind {
    UMDV_STREAM_VIDEO,
    UMDV_STREAM_AUDIO,
    UMDV_STREAM_SUBTITLE,
    UMDV_STREAM_OTHER
} umdv_stream_kind;

typedef struct umdv_stream {
    uint8_t stream_id;       /* PES stream ID: 0xE0 video, 0xBD private stream 1 */
    uint8_t sub_id;          /* private stream 1 substream: 0x00-0x0F audio, 0x80-0x9F subtitles */
    uint8_t kind;            /* umdv_stream_kind */
    char lang[3];            /* ISO 639-1, empty for video */
    uint8_t attr[8];         /* raw attributes, partly decoded */
} umdv_stream;

typedef struct umdv_ep {
    uint32_t pts;
    uint32_t sector;         /* 2048-byte sector inside the .MPS file */
    uint16_t flags;
} umdv_ep;

typedef struct umdv_clip {
    uint32_t start_pts;
    uint32_t end_pts;
    uint16_t stream_count;
    umdv_stream streams[UMDV_MAX_STREAMS];
    uint32_t ep_count;       /* video seek points, one per GOP */
    const uint8_t *ep_data;  /* points into the buffer given to umdv_clip_parse */
} umdv_clip;

/* The buffer must outlive the clip, because ep_data points into it. */
int umdv_clip_parse(const uint8_t *buf, uint32_t size, umdv_clip *out);
void umdv_clip_ep(const umdv_clip *c, uint32_t index, umdv_ep *out);
/* Index of the last seek point at or before pts, or 0. */
uint32_t umdv_clip_find(const umdv_clip *c, uint32_t pts);

#endif
