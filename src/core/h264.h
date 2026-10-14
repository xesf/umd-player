#ifndef H264_H
#define H264_H

#include <stdint.h>

/* Just enough H.264 header parsing for the player: the SPS (size, aspect, timing, field coding),
   the first slice header of each access unit (type, field flags, picture order count), and the
   pic_struct from the picture timing SEI. Decoding itself is the platform's job. */

#define H264_MAX_PARAM_SET 256

typedef struct h264_sps {
    int valid;
    int profile_idc, level_idc;
    int width, height;              /* after cropping */
    int frame_mbs_only;
    int log2_max_frame_num;
    int poc_type;
    int log2_max_poc_lsb;
    int max_num_ref_frames;
    int sar_w, sar_h;               /* 0:0 when not signalled */
    uint32_t num_units_in_tick, time_scale;
    int pic_struct_present;
    int cpb_dpb_delays_present;
    int cpb_removal_delay_len, dpb_output_delay_len, time_offset_len;
} h264_sps;

typedef enum h264_slice_type {
    H264_SLICE_P = 0,
    H264_SLICE_B = 1,
    H264_SLICE_I = 2
} h264_slice_type;

typedef struct h264_au_info {
    int has_sps;
    int is_idr;
    int nal_ref_idc;
    int slice_type;                 /* h264_slice_type */
    int frame_num;
    int field_pic, bottom_field;
    int poc_lsb;
    int poc;                        /* full picture order count, poc_type 0 only; resets at IDR */
    int pic_struct;                 /* from SEI, -1 when absent */
} h264_au_info;

typedef struct h264_parser {
    h264_sps sps;
    uint8_t sps_nal[H264_MAX_PARAM_SET], pps_nal[H264_MAX_PARAM_SET];
    uint32_t sps_size, pps_size;    /* latest SPS and PPS NAL units, without start codes */
    int prev_poc_msb, prev_poc_lsb; /* POC state, H.264 8.2.1.1 */
} h264_parser;

void h264_parser_init(h264_parser *p);

/* Parses one Annex B access unit. Returns 0 if a slice header was found. */
int h264_parse_au(h264_parser *p, const uint8_t *au, uint32_t size, h264_au_info *out);

/* Calls fn for each NAL unit in an Annex B buffer (data without the start code). */
typedef void (*h264_nal_fn)(void *user, const uint8_t *nal, uint32_t size);
void h264_each_nal(const uint8_t *buf, uint32_t size, h264_nal_fn fn, void *user);

#endif
