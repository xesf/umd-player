#ifndef DEMUX_H
#define DEMUX_H

#include <stdint.h>
#include "mpeg_ps.h"

/* Splits one .MPS clip into video access units, ATRAC3plus frames, and subtitle units.
   Layouts are in docs/formats/umd-video.md. One demuxer per playing clip; it owns all
   its buffers, so allocate it statically or on the heap (it is about 1 MiB). */

#define DEMUX_IN_SIZE    (128 * 1024)
#define DEMUX_VIDEO_SIZE (512 * 1024)
#define DEMUX_AUDIO_SIZE (16 * 1024)
#define DEMUX_SUB_SIZE   (256 * 1024)
#define DEMUX_PTS_QUEUE  16

#define DEMUX_NONE       (-1)
#define AT3P_SAMPLES     2048
#define AT3P_HEADER_SIZE 8

typedef enum demux_type {
    DEMUX_VIDEO = 1,     /* one H.264 access unit, Annex B, starting with its delimiter NAL */
    DEMUX_AUDIO,         /* one ATRAC3plus frame including its 8-byte 0F D0 header */
    DEMUX_SUBTITLE       /* one subtitle unit: 18-byte header then a PNG file */
} demux_type;

typedef struct demux_unit {
    uint8_t type;        /* demux_type */
    uint8_t sub_id;
    const uint8_t *data; /* valid until the next demux call */
    uint32_t size;
    uint64_t pts;        /* PS_NO_TS when the stream does not carry one for this unit */
    uint64_t dts;
} demux_unit;

/* Returns the bytes read, 0 at the end of the clip, negative on error. */
typedef int64_t (*demux_read_fn)(void *user, uint64_t offset, void *dst, uint32_t len);

/* A PTS from a PES header, tied to stream byte positions. Video: applies to the first
   access unit that starts in [pos, end). Audio: applies to the frame starting at pos. */
typedef struct demux_pts_mark {
    uint64_t pos;
    uint64_t end;
    uint64_t pts;
    uint64_t dts;
} demux_pts_mark;

typedef struct demux_stats {
    uint32_t video_dropped;  /* bytes dropped while resyncing */
    uint32_t audio_dropped;
    uint32_t sub_dropped;
    uint32_t pes_errors;
} demux_stats;

typedef struct demux {
    demux_read_fn read;
    void *user;
    uint64_t file_pos;       /* file offset of in[in_len] */
    uint32_t in_pos, in_len;
    int eof;

    int audio_sub;           /* selected substreams, or DEMUX_NONE */
    int sub_sub;

    uint32_t video_len, video_discard, video_scan;
    uint64_t video_pos;      /* stream byte position of video[0] */
    int video_synced;        /* video[0] starts an access unit */
    demux_pts_mark video_marks[DEMUX_PTS_QUEUE];
    uint32_t video_mark_count;

    uint32_t audio_len, audio_discard;
    uint64_t audio_pos;      /* stream byte position of audio[0] */
    uint64_t audio_last_pts;
    int audio_synced;
    demux_pts_mark audio_marks[DEMUX_PTS_QUEUE];
    uint32_t audio_mark_count;

    uint32_t sub_len, sub_discard;
    uint64_t sub_pts;
    int sub_open;

    demux_stats stats;

    uint8_t in[DEMUX_IN_SIZE];
    uint8_t video[DEMUX_VIDEO_SIZE];
    uint8_t audio[DEMUX_AUDIO_SIZE];
    uint8_t sub[DEMUX_SUB_SIZE];
} demux;

void demux_init(demux *d, demux_read_fn read, void *user);
/* audio_sub: 0x00-0x0F or DEMUX_NONE. sub_sub: 0x80-0x9F or DEMUX_NONE. */
void demux_select(demux *d, int audio_sub, int sub_sub);
/* Restarts at a 2048-byte sector, normally a seek point from the .CLP file. */
void demux_seek(demux *d, uint32_t sector);
/* Returns 1 with a unit, 0 at the end of the clip, negative on a read error. */
int demux_next(demux *d, demux_unit *out);

/* ATRAC3plus frame header helpers. */
uint32_t at3p_frame_size(const uint8_t *hdr);   /* header + data, or 0 if hdr is not a frame */

#endif
