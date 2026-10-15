#ifndef H264DEC_H
#define H264DEC_H

#include <stdint.h>

/* A software H.264 decoder for UMD Video: Main profile, 8-bit 4:2:0, CABAC, frame pictures
   and field pictures (PAFF). Enough for every disc tested; it reports what it does not support
   (CAVLC, MBAFF, long-term references, memory management commands, temporal direct) instead of
   guessing. Output is bit-exact with the standard's decoding process.

   Feed it Annex B access units in decoding order; take decoded frames in display order. A field
   pair comes out as one frame with the two fields interleaved. */

typedef struct h264dec h264dec;

typedef struct h264dec_frame {
    const uint8_t *plane[3];   /* Y, Cb, Cr */
    int stride[3];
    int width, height;         /* after cropping; chroma is half of each */
    int interlaced;            /* coded as two fields */
    int top_field_first;
    uint64_t pts;              /* as given to h264dec_decode with the frame's first picture */
    int poc;
} h264dec_frame;

h264dec *h264dec_open(void);
void h264dec_close(h264dec *d);

/* Decodes one access unit (one frame or one field). Returns 0, or -1 on a stream it cannot
   decode (the message is in h264dec_error). */
int h264dec_decode(h264dec *d, const uint8_t *au, uint32_t size, uint64_t pts);

/* The next frame ready for display, valid until the next call to the decoder; 0 when none
   is ready. After h264dec_flush every remaining frame comes out. */
int h264dec_output(h264dec *d, h264dec_frame *out);
void h264dec_flush(h264dec *d);

/* Forgets every picture, for a seek; decoding restarts at the next IDR or I picture. */
void h264dec_reset(h264dec *d);

const char *h264dec_error(const h264dec *d);

#endif
