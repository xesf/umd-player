/*
 * ATRAC3+ decoder front end for umd-player.
 *
 * Based on FFmpeg libavcodec/atrac3plusdec.c (n7.1):
 * Copyright (c) 2010-2013 Maxim Poliakovski
 *
 * Changes for umd-player (c) 2026 umd-player contributors: removed the AVCodec framework,
 * mono and stereo only, and added the VLC builder, float vector helpers, sine windows,
 * and an FFT-based inverse MDCT in place of the ones FFmpeg provides in other files.
 *
 * This file is part of the ATRAC3+ decoder in umd-player, which is derived from FFmpeg.
 * It is free software; you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 */

#include "at3p.h"

#include "ffcompat.h"
#include "atrac.h"
#include "atrac3plus.h"

/* ---- libavcodec/vlc.c: ff_vlc_init_from_lengths, single-level tables only ---- */

int ff_vlc_init_from_lengths(VLC *vlc, int nb_bits, int nb_codes, const uint8_t *lens, int lens_wrap,
                             const void *symbols, int symbols_wrap, int symbols_size, int offset,
                             int flags, void *logctx)
{
    const uint8_t *sym = (const uint8_t *)symbols;
    uint64_t code = 0;
    int i, size = 1 << nb_bits;

    (void)flags;
    (void)logctx;
    if (symbols_size != 1 || size > vlc->table_allocated)
        return AVERROR_INVALIDDATA;
    vlc->bits = nb_bits;
    vlc->table_size = size;
    for (i = 0; i < size; i++) {
        vlc->table[i].sym = -1;
        vlc->table[i].len = 0;
    }
    for (i = 0; i < nb_codes; i++, lens += lens_wrap) {
        int len = *lens, j, first, count;

        if (len == 0)
            continue;
        if (len > nb_bits || code >= (1ULL << 32))
            return AVERROR_INVALIDDATA;
        first = (int)(code >> (32 - nb_bits));
        count = 1 << (nb_bits - len);
        for (j = 0; j < count; j++) {
            vlc->table[first + j].sym = (int16_t)(sym[i * symbols_wrap] + offset);
            vlc->table[first + j].len = (int16_t)len;
        }
        code += 1ULL << (32 - len);
    }
    return 0;
}

/* ---- libavutil/float_dsp.c: C versions ---- */

static void vector_fmul_c(float *dst, const float *src0, const float *src1, int len)
{
    for (int i = 0; i < len; i++)
        dst[i] = src0[i] * src1[i];
}

static void vector_fmul_reverse_c(float *dst, const float *src0, const float *src1, int len)
{
    src1 += len - 1;
    for (int i = 0; i < len; i++)
        dst[i] = src0[i] * src1[-i];
}

static void vector_fmul_scalar_c(float *dst, const float *src, float mul, int len)
{
    for (int i = 0; i < len; i++)
        dst[i] = src[i] * mul;
}

static void vector_fmac_scalar_c(float *dst, const float *src, float mul, int len)
{
    for (int i = 0; i < len; i++)
        dst[i] += src[i] * mul;
}

const AVFloatDSPContext ff_float_dsp_c = {
    vector_fmul_c, vector_fmul_reverse_c, vector_fmul_scalar_c, vector_fmac_scalar_c
};

/* ---- libavcodec/sinewin: the two window sizes ATRAC3+ uses ---- */

float ff_sine_64[64];
float ff_sine_128[128];

void ff_init_ff_sine_windows(int index)
{
    float *w = index == 6 ? ff_sine_64 : index == 7 ? ff_sine_128 : NULL;
    int n = 1 << index;

    assert(w);
    for (int i = 0; i < n; i++)
        w[i] = sinf((i + 0.5) * (M_PI / (2.0 * n)));
}

/* ---- Inverse MDCT with av_tx conventions ----
   For M = len input coefficients and n = 2M, FFmpeg's av_tx inverse MDCT computes
     full[i] = -scale * sum_k in[k] * cos(pi / (2n) * (2i + 1 + n/2) * (2k + 1)),  0 <= i < n
   and the half transform returns full[M/2 .. M/2 + M). This was measured against libavutil 60
   with impulse inputs; the error is at float precision.

   full[] is a DCT-IV of size M, unfolded:
     c[j]     = sum_k in[k] * cos(pi / M * (j + 1/2) * (k + 1/2))
     full[i]  =  c[i + M/2]          for i <  M/2
     full[i]  = -c[3M/2 - 1 - i]     for M/2 <= i < 3M/2   (so half[j] = -c[M - 1 - j])
     full[i]  = -c[i - 3M/2]         for i >= 3M/2
   and the DCT-IV is computed with an M/2-point complex FFT:
     v[k] = (in[2k] + i * in[M-1-2k]) * exp(-i pi (4k + 1) / (4M)),  V = FFT(v),
     w[j] = V[j] * exp(-i pi j / M),  c[2j] = Re w[j],  c[M-1-2j] = -Im w[j]. */

#define TX_MAX_LEN 128
#define TX_MAX_FFT (TX_MAX_LEN / 2)

struct AVTXContext {
    int len;
    int full;
    float pre_re[TX_MAX_FFT], pre_im[TX_MAX_FFT];
    float post_re[TX_MAX_FFT], post_im[TX_MAX_FFT];  /* include the -scale gain */
    float tw_re[TX_MAX_FFT / 2], tw_im[TX_MAX_FFT / 2];
    uint8_t bitrev[TX_MAX_FFT];
};

static void fft_inplace(const AVTXContext *s, float *re, float *im, int n)
{
    for (int i = 0; i < n; i++) {
        int j = s->bitrev[i];
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int size = 2, step = n / 2; size <= n; size <<= 1, step >>= 1) {
        int half = size >> 1;
        for (int base = 0; base < n; base += size) {
            for (int k = 0; k < half; k++) {
                float wr = s->tw_re[k * step], wi = s->tw_im[k * step];
                int a = base + k, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

static void imdct_fast(AVTXContext *s, void *out_v, void *in_v, ptrdiff_t stride)
{
    const float *in = (const float *)in_v;
    float *out = (float *)out_v;
    float re[TX_MAX_FFT], im[TX_MAX_FFT], c[TX_MAX_LEN];
    int m = s->len, h = m / 2;

    (void)stride;
    for (int k = 0; k < h; k++) {
        float a = in[2 * k], b = in[m - 1 - 2 * k];
        re[k] = a * s->pre_re[k] - b * s->pre_im[k];
        im[k] = a * s->pre_im[k] + b * s->pre_re[k];
    }
    fft_inplace(s, re, im, h);
    for (int j = 0; j < h; j++) {
        c[2 * j] = re[j] * s->post_re[j] - im[j] * s->post_im[j];
        c[m - 1 - 2 * j] = -(re[j] * s->post_im[j] + im[j] * s->post_re[j]);
    }
    if (s->full) {
        for (int i = 0; i < h; i++)
            out[i] = c[i + h];
        for (int i = h; i < 3 * h; i++)
            out[i] = -c[3 * h - 1 - i];
        for (int i = 3 * h; i < 2 * m; i++)
            out[i] = -c[i - 3 * h];
    } else {
        for (int j = 0; j < m; j++)
            out[j] = -c[m - 1 - j];
    }
}

static void tx_init(AVTXContext *s, int len, float scale, int full)
{
    int h = len / 2, bits = 0;

    assert(len <= TX_MAX_LEN && (len & (len - 1)) == 0);
    s->len = len;
    s->full = full;
    while ((1 << bits) < h)
        bits++;
    for (int k = 0; k < h; k++) {
        double a = -M_PI * (4 * k + 1) / (4.0 * len), b = -M_PI * k / len;
        int r = 0;
        s->pre_re[k] = (float)cos(a);
        s->pre_im[k] = (float)sin(a);
        s->post_re[k] = (float)(-scale * cos(b));
        s->post_im[k] = (float)(-scale * sin(b));
        for (int i = 0; i < bits; i++)
            r |= ((k >> i) & 1) << (bits - 1 - i);
        s->bitrev[k] = (uint8_t)r;
    }
    for (int k = 0; k < h / 2; k++) {
        s->tw_re[k] = (float)cos(-2 * M_PI * k / h);
        s->tw_im[k] = (float)sin(-2 * M_PI * k / h);
    }
}

/* ---- libavcodec/atrac3plusdec.c ---- */

struct at3p_decoder {
    GetBitContext gb;
    const AVFloatDSPContext *fdsp;

    DECLARE_ALIGNED(32, float, samples)[2][ATRAC3P_FRAME_SAMPLES];  /* quantized MDCT spectrum */
    DECLARE_ALIGNED(32, float, mdct_buf)[2][ATRAC3P_FRAME_SAMPLES]; /* output of the IMDCT */
    DECLARE_ALIGNED(32, float, time_buf)[2][ATRAC3P_FRAME_SAMPLES]; /* output of the gain compensation */
    DECLARE_ALIGNED(32, float, outp_buf)[2][ATRAC3P_FRAME_SAMPLES];

    AtracGCContext gainc_ctx;
    AVTXContext mdct_ctx;
    AVTXContext ipqf_dct_ctx;

    Atrac3pChanUnitCtx ch_unit;
    int channels;
    int unit_type;
};

static AVOnce init_static_once = AV_ONCE_INIT;

static void init_static(void)
{
    ff_atrac_generate_tables();
    ff_atrac3p_init_vlcs();
    ff_atrac3p_init_dsp_static();
}

void at3p_init_tables(void)
{
    ff_thread_once(&init_static_once, init_static);
}

size_t at3p_decoder_size(void)
{
    return sizeof(at3p_decoder);
}

int at3p_init(at3p_decoder *d, int channels)
{
    if (channels != 1 && channels != 2)
        return AVERROR_INVALIDDATA;
    at3p_init_tables();
    memset(d, 0, sizeof(*d));
    d->fdsp = &ff_float_dsp_c;
    d->channels = channels;
    d->unit_type = channels == 1 ? CH_UNIT_MONO : CH_UNIT_STEREO;

    tx_init(&d->ipqf_dct_ctx, 16, 32.0f / 32768.0f, 0);
    tx_init(&d->mdct_ctx, 128, -1.0f, 1);
    ff_atrac_init_gain_compensation(&d->gainc_ctx, 6, 2);

    for (int ch = 0; ch < 2; ch++) {
        Atrac3pChanParams *c = &d->ch_unit.channels[ch];
        c->ch_num = ch;
        c->wnd_shape = &c->wnd_shape_hist[0][0];
        c->wnd_shape_prev = &c->wnd_shape_hist[1][0];
        c->gain_data = &c->gain_data_hist[0][0];
        c->gain_data_prev = &c->gain_data_hist[1][0];
        c->tones_info = &c->tones_info_hist[0][0];
        c->tones_info_prev = &c->tones_info_hist[1][0];
    }
    d->ch_unit.waves_info = &d->ch_unit.wave_synth_hist[0];
    d->ch_unit.waves_info_prev = &d->ch_unit.wave_synth_hist[1];
    return 0;
}

static void decode_residual_spectrum(at3p_decoder *ctx, Atrac3pChanUnitCtx *ch_unit,
                                     float out[2][ATRAC3P_FRAME_SAMPLES], int num_channels)
{
    int i, sb, ch, qu, nspeclines, RNG_index;
    float *dst, q;
    int16_t *src;
    /* calculate RNG table index for each subband */
    int sb_RNG_index[ATRAC3P_SUBBANDS] = { 0 };

    if (ch_unit->mute_flag) {
        for (ch = 0; ch < num_channels; ch++)
            memset(out[ch], 0, ATRAC3P_FRAME_SAMPLES * sizeof(*out[ch]));
        return;
    }

    for (qu = 0, RNG_index = 0; qu < ch_unit->used_quant_units; qu++)
        RNG_index += ch_unit->channels[0].qu_sf_idx[qu] +
                     ch_unit->channels[1].qu_sf_idx[qu];

    for (sb = 0; sb < ch_unit->num_coded_subbands; sb++, RNG_index += 128)
        sb_RNG_index[sb] = RNG_index & 0x3FC;

    /* inverse quant and power compensation */
    for (ch = 0; ch < num_channels; ch++) {
        /* clear channel's residual spectrum */
        memset(out[ch], 0, ATRAC3P_FRAME_SAMPLES * sizeof(*out[ch]));

        for (qu = 0; qu < ch_unit->used_quant_units; qu++) {
            src        = &ch_unit->channels[ch].spectrum[ff_atrac3p_qu_to_spec_pos[qu]];
            dst        = &out[ch][ff_atrac3p_qu_to_spec_pos[qu]];
            nspeclines = ff_atrac3p_qu_to_spec_pos[qu + 1] -
                         ff_atrac3p_qu_to_spec_pos[qu];

            if (ch_unit->channels[ch].qu_wordlen[qu] > 0) {
                q = ff_atrac3p_sf_tab[ch_unit->channels[ch].qu_sf_idx[qu]] *
                    ff_atrac3p_mant_tab[ch_unit->channels[ch].qu_wordlen[qu]];
                for (i = 0; i < nspeclines; i++)
                    dst[i] = src[i] * q;
            }
        }

        for (sb = 0; sb < ch_unit->num_coded_subbands; sb++)
            ff_atrac3p_power_compensation(ch_unit, (AVFloatDSPContext *)ctx->fdsp, ch, &out[ch][0],
                                          sb_RNG_index[sb], sb);
    }

    if (ch_unit->unit_type == CH_UNIT_STEREO) {
        for (sb = 0; sb < ch_unit->num_coded_subbands; sb++) {
            if (ch_unit->swap_channels[sb]) {
                for (i = 0; i < ATRAC3P_SUBBAND_SAMPLES; i++)
                    FFSWAP(float, out[0][sb * ATRAC3P_SUBBAND_SAMPLES + i],
                                  out[1][sb * ATRAC3P_SUBBAND_SAMPLES + i]);
            }

            /* flip coefficients' sign if requested */
            if (ch_unit->negate_coeffs[sb])
                for (i = 0; i < ATRAC3P_SUBBAND_SAMPLES; i++)
                    out[1][sb * ATRAC3P_SUBBAND_SAMPLES + i] = -(out[1][sb * ATRAC3P_SUBBAND_SAMPLES + i]);
        }
    }
}

static void reconstruct_frame(at3p_decoder *ctx, Atrac3pChanUnitCtx *ch_unit, int num_channels)
{
    AVFloatDSPContext *fdsp = (AVFloatDSPContext *)ctx->fdsp;
    int ch, sb;

    for (ch = 0; ch < num_channels; ch++) {
        for (sb = 0; sb < ch_unit->num_subbands; sb++) {
            /* inverse transform and windowing */
            ff_atrac3p_imdct(fdsp, &ctx->mdct_ctx, imdct_fast,
                             &ctx->samples[ch][sb * ATRAC3P_SUBBAND_SAMPLES],
                             &ctx->mdct_buf[ch][sb * ATRAC3P_SUBBAND_SAMPLES],
                             (ch_unit->channels[ch].wnd_shape_prev[sb] << 1) +
                             ch_unit->channels[ch].wnd_shape[sb], sb);

            /* gain compensation and overlapping */
            ff_atrac_gain_compensation(&ctx->gainc_ctx,
                                       &ctx->mdct_buf[ch][sb * ATRAC3P_SUBBAND_SAMPLES],
                                       &ch_unit->prev_buf[ch][sb * ATRAC3P_SUBBAND_SAMPLES],
                                       &ch_unit->channels[ch].gain_data_prev[sb],
                                       &ch_unit->channels[ch].gain_data[sb],
                                       ATRAC3P_SUBBAND_SAMPLES,
                                       &ctx->time_buf[ch][sb * ATRAC3P_SUBBAND_SAMPLES]);
        }

        /* zero unused subbands in both output and overlapping buffers */
        memset(&ch_unit->prev_buf[ch][ch_unit->num_subbands * ATRAC3P_SUBBAND_SAMPLES], 0,
               (ATRAC3P_SUBBANDS - ch_unit->num_subbands) * ATRAC3P_SUBBAND_SAMPLES * sizeof(float));
        memset(&ctx->time_buf[ch][ch_unit->num_subbands * ATRAC3P_SUBBAND_SAMPLES], 0,
               (ATRAC3P_SUBBANDS - ch_unit->num_subbands) * ATRAC3P_SUBBAND_SAMPLES * sizeof(float));

        /* resynthesize and add tonal signal */
        if (ch_unit->waves_info->tones_present ||
            ch_unit->waves_info_prev->tones_present) {
            for (sb = 0; sb < ch_unit->num_subbands; sb++)
                if (ch_unit->channels[ch].tones_info[sb].num_wavs ||
                    ch_unit->channels[ch].tones_info_prev[sb].num_wavs) {
                    ff_atrac3p_generate_tones(ch_unit, fdsp, ch, sb,
                                              &ctx->time_buf[ch][sb * 128]);
                }
        }

        /* subband synthesis and acoustic signal output */
        ff_atrac3p_ipqf(&ctx->ipqf_dct_ctx, imdct_fast,
                        &ch_unit->ipqf_ctx[ch], &ctx->time_buf[ch][0],
                        &ctx->outp_buf[ch][0]);
    }

    /* swap window shape and gain control buffers. */
    for (ch = 0; ch < num_channels; ch++) {
        FFSWAP(uint8_t *, ch_unit->channels[ch].wnd_shape,
               ch_unit->channels[ch].wnd_shape_prev);
        FFSWAP(AtracGainInfo *, ch_unit->channels[ch].gain_data,
               ch_unit->channels[ch].gain_data_prev);
        FFSWAP(Atrac3pWavesData *, ch_unit->channels[ch].tones_info,
               ch_unit->channels[ch].tones_info_prev);
    }

    FFSWAP(Atrac3pWaveSynthParams *, ch_unit->waves_info, ch_unit->waves_info_prev);
}

int at3p_decode(at3p_decoder *ctx, const uint8_t *data, uint32_t size, float *out[2])
{
    int ret, ch_unit_id, units = 0;

    init_get_bits8(&ctx->gb, data, (int)size);
    if (get_bits1(&ctx->gb))
        return AVERROR_INVALIDDATA;   /* invalid start bit */

    while (get_bits_left(&ctx->gb) >= 2 &&
           (ch_unit_id = (int)get_bits(&ctx->gb, 2)) != CH_UNIT_TERMINATOR) {
        if (ch_unit_id == CH_UNIT_EXTENSION)
            return AVERROR_PATCHWELCOME;
        if (units > 0 || ch_unit_id != ctx->unit_type)
            return AVERROR_INVALIDDATA;   /* frame does not match the channel configuration */

        ctx->ch_unit.unit_type = ch_unit_id;
        if ((ret = ff_atrac3p_decode_channel_unit(&ctx->gb, &ctx->ch_unit, ctx->channels, NULL)) < 0)
            return ret;

        decode_residual_spectrum(ctx, &ctx->ch_unit, ctx->samples, ctx->channels);
        reconstruct_frame(ctx, &ctx->ch_unit, ctx->channels);

        for (int i = 0; i < ctx->channels; i++)
            memcpy(out[i], ctx->outp_buf[i], ATRAC3P_FRAME_SAMPLES * sizeof(float));
        units++;
    }
    return units ? 0 : AVERROR_INVALIDDATA;
}

void at3p_to_s16(float *const in[2], int channels, int16_t *out)
{
    for (int i = 0; i < AT3P_FRAME_SAMPLES; i++) {
        for (int ch = 0; ch < channels; ch++) {
            float v = in[ch][i] * 32768.0f;
            long s = lrintf(v);
            *out++ = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
    }
}
