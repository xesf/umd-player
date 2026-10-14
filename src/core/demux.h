#ifndef DEMUX_H
#define DEMUX_H

#include <stdint.h>
#include "mpeg_ps.h"

/* Splits one .MPS clip into video access units, ATRAC3plus frames, and subtitle units.
   Layouts are in docs/formats/umd-video.md. Any set of audio and subtitle tracks can be
   selected at once, so a player can switch tracks without reading the clip again; units of
   different tracks can interleave, so each track has its own assembly state. One demuxer per
   playing clip; it is about 1.3 MiB plus 256 KiB for each selected subtitle track. */

#define DEMUX_IN_SIZE    (128 * 1024)
#define DEMUX_VIDEO_SIZE (512 * 1024)
#define DEMUX_AUDIO_SIZE (16 * 1024)
#define DEMUX_SUB_SIZE   (256 * 1024)
#define DEMUX_PTS_QUEUE  16
#define DEMUX_AUDIO_TRACKS 16      /* substreams 0x00-0x0F */
#define DEMUX_SUB_TRACKS   32      /* substreams 0x80-0x9F */
#define DEMUX_SUB_FIRST    0x80

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

    uint32_t video_len, video_discard, video_scan;
    uint64_t video_pos;      /* stream byte position of video[0] */
    int video_synced;        /* video[0] starts an access unit */
    demux_pts_mark video_marks[DEMUX_PTS_QUEUE];
    uint32_t video_mark_count;

    uint32_t audio_mask;     /* selected audio substreams, bit n = 0x0n */
    uint32_t sub_mask;       /* selected subtitle substreams, bit n = 0x80 + n */
    int next_audio, next_sub;  /* round-robin start, so no track starves the others */

    struct demux_audio {
        uint32_t len, discard;
        uint64_t pos;        /* stream byte position of buf[0] */
        uint64_t last_pts;
        int synced;
        demux_pts_mark marks[DEMUX_PTS_QUEUE];
        uint32_t mark_count;
        uint8_t buf[DEMUX_AUDIO_SIZE];
    } audio[DEMUX_AUDIO_TRACKS];

    struct demux_sub {
        uint32_t len, discard;
        uint64_t pts;
        int open;
        uint8_t *buf;        /* DEMUX_SUB_SIZE, allocated when the track is first selected */
    } sub[DEMUX_SUB_TRACKS];

    demux_stats stats;

    uint8_t in[DEMUX_IN_SIZE];
    uint8_t video[DEMUX_VIDEO_SIZE];
} demux;

void demux_init(demux *d, demux_read_fn read, void *user);
/* Frees subtitle buffers. The demuxer can be initialized again afterwards. */
void demux_free(demux *d);
/* One audio and one subtitle track: 0x00-0x0F and 0x80-0x9F, or DEMUX_NONE. */
void demux_select(demux *d, int audio_sub, int sub_sub);
/* Any set of tracks: bit n of audio_mask is substream n, bit n of sub_mask is 0x80 + n.
   Returns -1 if a subtitle buffer cannot be allocated. Call between units, not concurrently. */
int demux_select_mask(demux *d, uint32_t audio_mask, uint32_t sub_mask);
/* Restarts at a 2048-byte sector, normally a seek point from the .CLP file. */
void demux_seek(demux *d, uint32_t sector);
/* Returns 1 with a unit, 0 at the end of the clip, negative on a read error. */
int demux_next(demux *d, demux_unit *out);

/* ATRAC3plus frame header helpers. */
uint32_t at3p_frame_size(const uint8_t *hdr);   /* header + data, or 0 if hdr is not a frame */

#endif
