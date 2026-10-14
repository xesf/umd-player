/*
 * Minimal replacements for the FFmpeg framework used by the ATRAC3+ decoder.
 *
 * Copyright (c) 2026 umd-player contributors
 *
 * This file is part of the ATRAC3+ decoder in umd-player, which is derived from FFmpeg.
 * It is free software; you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef AT3P_FFCOMPAT_H
#define AT3P_FFCOMPAT_H

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define av_cold
#define av_unused
#define AV_LOG_ERROR 0
#define av_log(ctx, level, ...) ((void)(ctx))
#define avpriv_report_missing_feature(ctx, ...) ((void)(ctx))
#define av_assert0(cond) assert(cond)

#define AVERROR_INVALIDDATA  (-1)
#define AVERROR_PATCHWELCOME (-2)

#define FFSWAP(type, a, b) do { type ffswap_tmp_ = (b); (b) = (a); (a) = ffswap_tmp_; } while (0)
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

#define DECLARE_ALIGNED(n, t, v) t __attribute__((aligned(n))) v
#define LOCAL_ALIGNED(a, t, v, ...) t __attribute__((aligned(a))) v __VA_ARGS__
#define LOCAL_ALIGNED_32(t, v, ...) LOCAL_ALIGNED(32, t, v, __VA_ARGS__)

static inline int av_log2(unsigned v)
{
    return v ? 31 - __builtin_clz(v) : 0;
}

static inline int sign_extend(int val, unsigned bits)
{
    unsigned shift = 32 - bits;
    return (int)((unsigned)val << shift) >> shift;
}

static inline unsigned av_zero_extend(unsigned a, unsigned p)
{
    return a & ((1U << p) - 1);
}

/* One-time init. The decoder calls these from at3p_init_tables(), before any thread starts. */
typedef int AVOnce;
#define AV_ONCE_INIT 0
static inline void ff_thread_once(AVOnce *once, void (*fn)(void))
{
    if (!*once) {
        *once = 1;
        fn();
    }
}

/* The decoder never looks inside the codec context; it is only passed to av_log. */
typedef struct AVCodecContext AVCodecContext;

/* Bit reader. Reads past the end return zero bits, like FFmpeg's zero-padded packets. */
typedef struct GetBitContext {
    const uint8_t *buffer;
    int index;
    int size_in_bits;
} GetBitContext;

static inline int init_get_bits8(GetBitContext *s, const uint8_t *buffer, int byte_size)
{
    s->buffer = buffer;
    s->index = 0;
    s->size_in_bits = byte_size * 8;
    return 0;
}

static inline unsigned show_bits_long(const GetBitContext *s, int n)
{
    uint64_t w = 0;
    int byte = s->index >> 3, i;

    if (n == 0)
        return 0;
    for (i = 0; i < 5; i++) {
        int b = byte + i;
        w = (w << 8) | (b < (s->size_in_bits >> 3) && b >= 0 ? s->buffer[b] : 0);
    }
    return (unsigned)((w >> (40 - (s->index & 7) - n)) & ((1ULL << n) - 1));
}

static inline unsigned get_bits(GetBitContext *s, int n)
{
    unsigned v = show_bits_long(s, n);
    s->index += n;
    return v;
}

static inline unsigned get_bitsz(GetBitContext *s, int n)
{
    return n ? get_bits(s, n) : 0;
}

static inline unsigned get_bits1(GetBitContext *s)
{
    return get_bits(s, 1);
}

static inline void skip_bits(GetBitContext *s, int n)
{
    s->index += n;
}

static inline int get_bits_left(const GetBitContext *s)
{
    return s->size_in_bits - s->index;
}

/* Single-level VLC tables, as used by ATRAC3+ (max_depth is always 1). */
typedef struct VLCElem {
    int16_t sym;
    int16_t len;
} VLCElem;

typedef struct VLC {
    int bits;
    VLCElem *table;
    int table_size;
    int table_allocated;
} VLC;

#define VLC_INIT_USE_STATIC 1

/* Codes are assigned in list order, like FFmpeg: each entry takes the next code of its length. */
int ff_vlc_init_from_lengths(VLC *vlc, int nb_bits, int nb_codes, const uint8_t *lens, int lens_wrap,
                             const void *symbols, int symbols_wrap, int symbols_size, int offset,
                             int flags, void *logctx);

static inline int get_vlc2(GetBitContext *s, const VLCElem *table, int bits, int max_depth)
{
    const VLCElem *e = &table[show_bits_long(s, bits)];
    (void)max_depth;
    s->index += e->len;
    return e->sym;
}

/* Float vector helpers (libavutil/float_dsp.h). */
typedef struct AVFloatDSPContext {
    void (*vector_fmul)(float *dst, const float *src0, const float *src1, int len);
    void (*vector_fmul_reverse)(float *dst, const float *src0, const float *src1, int len);
    void (*vector_fmul_scalar)(float *dst, const float *src, float mul, int len);
    void (*vector_fmac_scalar)(float *dst, const float *src, float mul, int len);
} AVFloatDSPContext;

extern const AVFloatDSPContext ff_float_dsp_c;

/* Inverse MDCT with FFmpeg's av_tx conventions; see at3p.c. */
typedef struct AVTXContext AVTXContext;
typedef void (*av_tx_fn)(AVTXContext *s, void *out, void *in, ptrdiff_t stride);

/* Sine windows (libavcodec/sinewin.h). Only the sizes ATRAC3+ uses. */
extern float ff_sine_64[64];
extern float ff_sine_128[128];
void ff_init_ff_sine_windows(int index);

#endif
