#ifndef MPEG_PS_H
#define MPEG_PS_H

#include <stdint.h>

/* MPEG-2 program stream parser for .MPS files. Works on a caller-owned buffer;
   returned pointers stay valid until the caller reuses that buffer. */

#define PS_STREAM_PRIVATE1 0xBD
#define PS_STREAM_PADDING  0xBE
#define PS_STREAM_PRIVATE2 0xBF
#define PS_STREAM_VIDEO0   0xE0

#define PS_NO_TS UINT64_MAX

typedef struct ps_packet {
    uint8_t stream_id;
    uint8_t sub_id;          /* first payload byte of private stream 1, else 0 */
    uint64_t pts;            /* 90 kHz, PS_NO_TS when absent */
    uint64_t dts;
    const uint8_t *data;     /* payload after the PES header; for 0xBD it starts at sub_id */
    uint32_t size;
    uint32_t offset;         /* byte offset of the 00 00 01 start code in the buffer */
} ps_packet;

enum {
    PS_OK = 1,               /* *out holds a PES packet */
    PS_NEED_MORE = 0,        /* the packet at *pos runs past len; refill from *pos */
    PS_ERROR = -1            /* malformed packet; *pos is past its start code */
};

/* Skips pack headers, system headers, end codes, and padding. Resyncs on garbage. */
int ps_next(const uint8_t *buf, uint32_t len, uint32_t *pos, ps_packet *out);

#endif
