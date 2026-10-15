#ifndef H264DEC_INT_H
#define H264DEC_INT_H

/* Internals of the H.264 decoder (h264dec.h), shared by its source files:
   h264dec.c          NAL units, parameter sets, slice headers, picture order, references, output
   h264dec_slice.c    CABAC and the macroblock layer: syntax, motion vectors, direct prediction
   h264dec_recon.c    intra prediction, motion compensation, inverse transforms
   h264dec_deblock.c  the loop filter
   Section numbers refer to ITU-T H.264. */

#include <stdint.h>

#include "h264dec.h"

#define MAX_SPS       32
#define MAX_PPS       256
#define DPB_FRAMES    18          /* 16 references, the current picture, and one waiting */
#define MAX_LIST      33          /* reference list entries: up to 32 fields, plus a spare */

typedef struct dec_sps {
    int valid;
    int profile_idc, level_idc;
    int chroma_format_idc;
    int log2_max_frame_num;
    int poc_type, log2_max_poc_lsb;
    int max_num_ref_frames;
    int mb_w, mb_h;                /* frame size in macroblocks */
    int frame_mbs_only, mbaff, direct_8x8_inference;
    int crop_left, crop_right, crop_top, crop_bottom;   /* in luma samples */
    int num_reorder_frames;        /* from the VUI, or -1 */
} dec_sps;

typedef struct dec_pps {
    int valid;
    int sps_id;
    int cabac;
    int bottom_field_pic_order_in_frame_present;
    int num_ref_idx_default[2];
    int weighted_pred, weighted_bipred_idc;
    int pic_init_qp;
    int chroma_qp_index_offset;
    int deblocking_filter_control_present;
    int constrained_intra_pred;
    int redundant_pic_cnt_present;
} dec_pps;

enum { SLICE_P = 0, SLICE_B = 1, SLICE_I = 2 };

typedef struct dec_slice {
    int nal_ref_idc, idr;
    int first_mb;
    int type;                      /* SLICE_P, SLICE_B, SLICE_I */
    int pps_id;
    int frame_num;
    int field_pic, bottom_field;
    int idr_pic_id;
    int poc_lsb, delta_poc_bottom;
    int direct_spatial;
    int num_ref[2];
    int cabac_init_idc;
    int qp;
    int disable_deblock;           /* disable_deblocking_filter_idc */
    int alpha_offset, beta_offset; /* FilterOffsetA and FilterOffsetB */
    /* explicit weighted prediction */
    int luma_log2_wd, chroma_log2_wd;
    int16_t luma_w[2][MAX_LIST], luma_o[2][MAX_LIST];
    int16_t chroma_w[2][MAX_LIST][2], chroma_o[2][MAX_LIST][2];
    int8_t weighted[2][MAX_LIST];  /* 1 when this entry has explicit weights */
} dec_slice;

/* What later pictures and the loop filter need from a decoded macroblock. Field pictures keep
   their macroblocks apart: the top field's rows first, then the bottom field's. */
typedef struct mbinfo {
    int8_t intra;                  /* 0 inter, 1 intra, 2 I_PCM */
    int8_t qp;                     /* QPY */
    int8_t qpc;                    /* QPC */
    int8_t deblock;                /* disable_deblocking_filter_idc of its slice */
    int8_t alpha, beta;            /* filter offsets of its slice */
    int16_t slice;                 /* slice number within the picture */
    uint16_t nz;                   /* luma 4x4 blocks with coefficients, bit y * 4 + x */
    int8_t ref[2][4];              /* refIdx per 8x8 block, -1 when the list is unused */
    int32_t refpic[2][4];          /* the picture behind each refIdx, for the loop filter */
    int16_t mv[2][16][2];          /* per 4x4 block, raster order */
} mbinfo;

typedef struct picture {
    int used;                      /* holds a picture: decoding, a reference, or waiting for output */
    uint8_t *plane[3];
    int stride[3];
    mbinfo *mb;
    int coded_frame;               /* decoded as a frame picture (not as two fields) */
    int fields;                    /* decoded fields: 1 top, 2 bottom */
    int ref;                       /* fields marked "used for short-term reference": 1 top, 2 bottom */
    int frame_num, frame_num_wrap;
    int poc_top, poc_bot;
    int output_needed;
    int top_field_first;
    uint64_t pts;
    int id;
    int epoch;                     /* IDR pictures counted so far: output goes epoch by epoch */
} picture;

/* A reference list entry: a frame (both fields) or one field of a frame store. */
typedef struct refpic {
    picture *pic;
    int parity;                    /* 3 frame, 1 top field, 2 bottom field */
} refpic;

/* Neighbour state for CABAC context selection, for the picture being decoded. */
enum { MB_I4 = 1, MB_I16, MB_PCM, MB_P, MB_B, MB_BDIRECT };   /* MB_BDIRECT: B_Skip, B_Direct_16x16 */

typedef struct mbctx {
    int16_t slice;                 /* slice number + 1; 0 before the macroblock is decoded */
    uint8_t skip, type;
    uint8_t cbp;                   /* luma bits 0-3, chroma 4-5 */
    uint8_t chroma_mode;
    uint8_t cbf_dc;                /* coded_block_flag of luma DC (1), Cb DC (2), Cr DC (4) */
    uint16_t cbf_luma;             /* per 4x4 block, bit y * 4 + x */
    uint8_t cbf_ac[2];             /* chroma AC per 4x4 block, bit y * 2 + x */
    int8_t i4mode[16];             /* raster; 2 for macroblocks that are not Intra_4x4 */
    uint8_t mvd[2][16][2];         /* absolute values, capped */
    uint8_t direct;                /* 8x8 blocks predicted in direct mode, bit per block */
} mbctx;

typedef struct cabac {
    const uint8_t *p, *end;
    uint32_t range, offset;
    int bits_left;                 /* bits of *p not yet read */
    uint8_t state[460];            /* (pStateIdx << 1) | valMPS */
} cabac;

struct h264dec {
    dec_sps sps[MAX_SPS];
    dec_pps pps[MAX_PPS];
    const dec_sps *cur_sps;
    const dec_pps *cur_pps;
    dec_slice sh;

    picture dpb[DPB_FRAMES];
    int next_pic_id;
    int mb_w, mb_h;                /* frame size of the allocated pictures */

    /* the picture being decoded */
    picture *cur;
    int structure;                 /* 3 frame, 1 top field, 2 bottom field */
    int pic_mb_h;                  /* macroblock rows of the current picture */
    int poc;                       /* of the current picture (field or frame) */
    uint64_t pts;
    uint8_t *dst[3];               /* top-left of the current picture or field */
    int dst_stride[3];
    mbinfo *mbi;                   /* macroblock info of the current picture or field */
    mbctx *ctx;
    int slice_num;
    int decoding;                  /* a picture is open */

    /* picture order count and frame number state, 8.2.1 */
    int prev_poc_msb, prev_poc_lsb;
    int prev_ref_frame_num;
    int max_frame_num;

    refpic list[2][MAX_LIST];
    int list_len[2];

    /* macroblock decoding */
    cabac cab;
    int qp;
    int last_dqp;                  /* the previous macroblock had a nonzero mb_qp_delta */
    uint8_t *rbsp;
    uint32_t rbsp_cap;

    /* output */
    int num_reorder;
    h264dec_frame out_frame;
    int flushing;
    int epoch;
    int need_key;                  /* after a reset: skip until an I or IDR picture */
    int recovering;                /* the pictures before that I picture in display order are */
    int recover_epoch, recover_poc;/* missing references: drop them */

    char err[128];
};

/* h264dec.c */
int h264dec_fail(h264dec *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int h264dec_ref_poc(const refpic *r);

/* h264dec_slice.c */
int h264dec_decode_slice_data(h264dec *d, const uint8_t *data, uint32_t size, uint32_t bit_offset);

/* h264dec_recon.c */
void h264dec_intra4x4(uint8_t *dst, int stride, int mode, const uint8_t *top, const uint8_t *left,
                      uint8_t topleft, int have_top, int have_left, int have_topleft, int have_topright);
void h264dec_intra16x16(uint8_t *dst, int stride, int mode, int have_top, int have_left, int have_topleft);
void h264dec_intra_chroma(uint8_t *dst, int stride, int mode, int have_top, int have_left, int have_topleft);
void h264dec_idct4x4_add(uint8_t *dst, int stride, int16_t *c);
void h264dec_mc_luma(uint8_t *dst, int dst_stride, const uint8_t *ref, int ref_stride, int ref_w, int ref_h,
                     int x, int y, int mvx, int mvy, int w, int h);
void h264dec_mc_chroma(uint8_t *dst, int dst_stride, const uint8_t *ref, int ref_stride, int ref_w, int ref_h,
                       int x, int y, int mvx, int mvy, int w, int h);

/* h264dec_deblock.c */
void h264dec_deblock_picture(h264dec *d);

static inline int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint8_t clip1(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

#endif
