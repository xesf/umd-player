#ifndef H264_PROGRESSIVE_H
#define H264_PROGRESSIVE_H

#include <stdint.h>

/* VideoToolbox rejects H.264 with frame_mbs_only_flag = 0, which every UMD Video disc sets,
   even the films that only use frame pictures without MBAFF. For those, the stream decodes the
   same as a progressive one, so this rewrites:
   - the SPS: frame_mbs_only_flag = 1, no mb_adaptive_frame_field_flag, height in map units doubled;
   - each slice header: field_pic_flag removed, CABAC alignment redone.
   Field pictures cannot be rewritten; they only play on the Vita. */

#define H264P_MAX_PPS 32

typedef struct h264p_state {
    int have_sps;
    int frame_mbs_only, mbaff;
    int chroma_format_idc, separate_colour_plane;
    int log2_max_frame_num, poc_type, log2_max_poc_lsb, delta_pic_order_always_zero;
    struct {
        int valid, cabac, bottom_field_pic_order, num_ref_idx_l0, num_ref_idx_l1;
        int weighted_pred, weighted_bipred_idc, deblocking_control, redundant_pic_cnt;
    } pps[H264P_MAX_PPS];
} h264p_state;

enum {
    H264P_COPY = 0,       /* not changed: use the input NAL as it is */
    H264P_REWRITTEN = 1,  /* *out_size bytes in out */
    H264P_FIELD = -1,     /* a field picture: cannot be made progressive */
    H264P_ERROR = -2
};

/* nal is one NAL unit without its start code. out needs 2 * size + 16 bytes. */
int h264p_rewrite(h264p_state *s, const uint8_t *nal, uint32_t size, uint8_t *out, uint32_t *out_size);

#endif
