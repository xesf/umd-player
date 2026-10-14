/*
 * ATRAC3+ decoder interface for umd-player.
 *
 * This file is part of the ATRAC3+ decoder in umd-player, which is derived from FFmpeg.
 * It is free software; you can redistribute it and/or modify it under the terms of the
 * GNU Lesser General Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef AT3P_H
#define AT3P_H

#include <stddef.h>
#include <stdint.h>

#define AT3P_FRAME_SAMPLES 2048

typedef struct at3p_decoder at3p_decoder;

/* Builds the shared VLC and DSP tables. Call once, before any decoder thread starts. */
void at3p_init_tables(void);

/* The decoder is about 90 KiB; the caller owns the memory (static, heap, or arena). */
size_t at3p_decoder_size(void);

/* channels: 1 or 2. Returns 0 on success. */
int at3p_init(at3p_decoder *d, int channels);

/* Decodes one frame: the frame data without the 8-byte 0F D0 header.
   Writes AT3P_FRAME_SAMPLES floats per channel, nominal range -1..1. Returns 0 on success. */
int at3p_decode(at3p_decoder *d, const uint8_t *data, uint32_t size, float *out[2]);

/* Converts planar float output to interleaved 16-bit PCM, with rounding and clipping. */
void at3p_to_s16(float *const in[2], int channels, int16_t *out);

#endif
