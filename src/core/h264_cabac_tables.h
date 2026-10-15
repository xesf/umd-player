#ifndef H264_CABAC_TABLES_H
#define H264_CABAC_TABLES_H

#include <stdint.h>

#define H264_CABAC_CTX 460    /* ctxIdx 0..459: everything the Main profile uses */

/* [ctxIdx][0 for I slices, else cabac_init_idc + 1][m, n] */
extern const int8_t h264_cabac_init[H264_CABAC_CTX][4][2];

/* The arithmetic decoding engine: codIRangeLPS by pStateIdx and qCodIRangeIdx, and the state
   transitions after a least or most probable symbol. */
extern const uint8_t h264_cabac_range_lps[64][4];
extern const uint8_t h264_cabac_next_lps[64];
extern const uint8_t h264_cabac_next_mps[64];

#endif
