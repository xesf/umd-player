/*
 * common functions for the ATRAC family of decoders
 *
 * Copyright (c) 2006-2013 Maxim Poliakovski
 * Copyright (c) 2006-2008 Benjamin Larsson
 *
 * Modified for umd-player on 2026-10-14: replaced FFmpeg framework includes with ffcompat.h, removed the unused ATRAC1/ATRAC3 QMF filter.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 */

#include <math.h>
#include <string.h>

#include "ffcompat.h"  /* umd-player: replaces the FFmpeg framework headers */

#include "atrac.h"

float ff_atrac_sf_table[64];

static av_cold void atrac_generate_tables(void)
{
    /* Generate scale factors */
    for (int i = 0; i < 64; i++)
        ff_atrac_sf_table[i] = pow(2.0, (i - 15) / 3.0);
}

av_cold void ff_atrac_generate_tables(void)
{
    static AVOnce init_static_once = AV_ONCE_INIT;
    ff_thread_once(&init_static_once, atrac_generate_tables);
}

av_cold void ff_atrac_init_gain_compensation(AtracGCContext *gctx, int id2exp_offset,
                                             int loc_scale)
{
    int i;

    gctx->loc_scale     = loc_scale;
    gctx->loc_size      = 1 << loc_scale;
    gctx->id2exp_offset = id2exp_offset;

    /* Generate gain level table. */
    for (i = 0; i < 16; i++)
        gctx->gain_tab1[i] = powf(2.0, id2exp_offset - i);

    /* Generate gain interpolation table. */
    for (i = -15; i < 16; i++)
        gctx->gain_tab2[i + 15] = powf(2.0, -1.0f / gctx->loc_size * i);
}

void ff_atrac_gain_compensation(AtracGCContext *gctx, float *in, float *prev,
                                AtracGainInfo *gc_now, AtracGainInfo *gc_next,
                                int num_samples, float *out)
{
    float lev, gc_scale, gain_inc;
    int i, pos, lastpos;

    gc_scale = gc_next->num_points ? gctx->gain_tab1[gc_next->lev_code[0]]
                                   : 1.0f;

    if (!gc_now->num_points) {
        for (pos = 0; pos < num_samples; pos++)
            out[pos] = in[pos] * gc_scale + prev[pos];
    } else {
        pos = 0;

        for (i = 0; i < gc_now->num_points; i++) {
            lastpos = gc_now->loc_code[i] << gctx->loc_scale;

            lev = gctx->gain_tab1[gc_now->lev_code[i]];
            gain_inc = gctx->gain_tab2[(i + 1 < gc_now->num_points ? gc_now->lev_code[i + 1]
                                                                   : gctx->id2exp_offset) -
                                       gc_now->lev_code[i] + 15];

            /* apply constant gain level and overlap */
            for (; pos < lastpos; pos++)
                out[pos] = (in[pos] * gc_scale + prev[pos]) * lev;

            /* interpolate between two different gain levels */
            for (; pos < lastpos + gctx->loc_size; pos++) {
                out[pos] = (in[pos] * gc_scale + prev[pos]) * lev;
                lev *= gain_inc;
            }
        }

        for (; pos < num_samples; pos++)
            out[pos] = in[pos] * gc_scale + prev[pos];
    }

    /* copy the overlapping part into the delay buffer */
    memcpy(prev, &in[num_samples], num_samples * sizeof(float));
}
