#include "h264_progressive.h"

#include <stdlib.h>
#include <string.h>

#define NAL_SLICE 1
#define NAL_IDR   5
#define NAL_SPS   7
#define NAL_PPS   8

typedef struct { const uint8_t *buf; uint32_t bits, pos; } reader;
typedef struct { uint8_t *buf; uint32_t cap_bits, pos; int overflow; } writer;

static uint32_t rd(reader *r, int n)
{
    uint32_t v = 0;
    while (n-- > 0) {
        uint32_t bit = r->pos < r->bits ? (r->buf[r->pos >> 3] >> (7 - (r->pos & 7))) & 1 : 0;
        v = (v << 1) | bit;
        r->pos++;
    }
    return v;
}

static uint32_t rd_ue(reader *r)
{
    int zeros = 0;
    while (rd(r, 1) == 0 && zeros < 32)
        zeros++;
    return zeros ? ((1u << zeros) - 1) + rd(r, zeros) : 0;
}

static int32_t rd_se(reader *r)
{
    uint32_t v = rd_ue(r);
    return v & 1 ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
}

static void wr(writer *w, uint32_t v, int n)
{
    while (n-- > 0) {
        if (w->pos >= w->cap_bits) {
            w->overflow = 1;
            return;
        }
        if ((v >> n) & 1)
            w->buf[w->pos >> 3] |= (uint8_t)(0x80 >> (w->pos & 7));
        else
            w->buf[w->pos >> 3] &= (uint8_t)~(0x80 >> (w->pos & 7));
        w->pos++;
    }
}

static void wr_ue(writer *w, uint32_t v)
{
    uint64_t x = (uint64_t)v + 1;
    int len = 0;
    while ((x >> (len + 1)) != 0)
        len++;
    wr(w, 0, len);
    wr(w, (uint32_t)x, len + 1);
}

static void wr_se(writer *w, int32_t v)
{
    wr_ue(w, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * (int64_t)v));
}

static uint32_t cu(reader *r, writer *w, int n) { uint32_t v = rd(r, n); wr(w, v, n); return v; }
static uint32_t cue(reader *r, writer *w) { uint32_t v = rd_ue(r); wr_ue(w, v); return v; }
static int32_t cse(reader *r, writer *w) { int32_t v = rd_se(r); wr_se(w, v); return v; }

/* Position of the rbsp_stop_one_bit: the last 1 bit of the payload. */
static uint32_t stop_bit(const reader *r)
{
    uint32_t byte = r->bits >> 3;
    while (byte > 0 && r->buf[byte - 1] == 0)
        byte--;
    if (byte == 0)
        return 0;
    for (int b = 0; b < 8; b++)
        if (r->buf[byte - 1] & (1 << b))
            return (byte - 1) * 8 + (7 - b);
    return 0;
}

/* Copies the rest of the payload up to the stop bit, then writes new trailing bits. */
static void copy_tail(reader *r, writer *w)
{
    uint32_t end = stop_bit(r);
    while (r->pos < end) {
        int n = end - r->pos > 24 ? 24 : (int)(end - r->pos);
        cu(r, w, n);
    }
    wr(w, 1, 1);
    while (w->pos & 7)
        wr(w, 0, 1);
}

static void copy_scaling_list(reader *r, writer *w, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; j++) {
        if (next != 0)
            next = (last + cse(r, w) + 256) % 256;
        last = next == 0 ? last : next;
    }
}

static int rewrite_sps(h264p_state *s, reader *r, writer *w)
{
    int profile, chroma = 1;
    uint32_t height, crop;

    profile = (int)cu(r, w, 8);
    cu(r, w, 16);
    cue(r, w);
    s->separate_colour_plane = 0;
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
        chroma = (int)cue(r, w);
        if (chroma == 3)
            s->separate_colour_plane = (int)cu(r, w, 1);
        cue(r, w);
        cue(r, w);
        cu(r, w, 1);
        if (cu(r, w, 1))
            for (int i = 0; i < (chroma != 3 ? 8 : 12); i++)
                if (cu(r, w, 1))
                    copy_scaling_list(r, w, i < 6 ? 16 : 64);
        break;
    default:
        break;
    }
    s->chroma_format_idc = chroma;
    s->log2_max_frame_num = (int)cue(r, w) + 4;
    s->poc_type = (int)cue(r, w);
    s->delta_pic_order_always_zero = 0;
    if (s->poc_type == 0) {
        s->log2_max_poc_lsb = (int)cue(r, w) + 4;
    } else if (s->poc_type == 1) {
        uint32_t n;
        s->delta_pic_order_always_zero = (int)cu(r, w, 1);
        cse(r, w);
        cse(r, w);
        n = cue(r, w);
        for (uint32_t i = 0; i < n && i < 256; i++)
            cse(r, w);
    }
    cue(r, w);
    cu(r, w, 1);
    cue(r, w);
    height = rd_ue(r);
    s->frame_mbs_only = (int)rd(r, 1);
    s->mbaff = s->frame_mbs_only ? 0 : (int)rd(r, 1);
    if (s->mbaff)
        return H264P_FIELD;
    /* Map units were field macroblock pairs; as frame macroblocks there are twice as many rows. */
    wr_ue(w, s->frame_mbs_only ? height : 2 * height + 1);
    wr(w, 1, 1);
    cu(r, w, 1);
    crop = cu(r, w, 1);
    if (crop) {
        cue(r, w);
        cue(r, w);
        /* The vertical crop unit halves with frame_mbs_only_flag = 1. */
        wr_ue(w, rd_ue(r) * (s->frame_mbs_only ? 1 : 2));
        wr_ue(w, rd_ue(r) * (s->frame_mbs_only ? 1 : 2));
    }
    copy_tail(r, w);
    s->have_sps = 1;
    return H264P_REWRITTEN;
}

static void parse_pps(h264p_state *s, reader *r)
{
    uint32_t id = rd_ue(r);

    if (id >= H264P_MAX_PPS)
        return;
    rd_ue(r);
    s->pps[id].cabac = (int)rd(r, 1);
    s->pps[id].bottom_field_pic_order = (int)rd(r, 1);
    if (rd_ue(r) != 0) {   /* slice groups: Baseline only, never on UMD */
        s->pps[id].valid = 0;
        return;
    }
    s->pps[id].num_ref_idx_l0 = (int)rd_ue(r) + 1;
    s->pps[id].num_ref_idx_l1 = (int)rd_ue(r) + 1;
    s->pps[id].weighted_pred = (int)rd(r, 1);
    s->pps[id].weighted_bipred_idc = (int)rd(r, 2);
    rd_se(r);
    rd_se(r);
    rd_se(r);
    s->pps[id].deblocking_control = (int)rd(r, 1);
    rd(r, 1);
    s->pps[id].redundant_pic_cnt = (int)rd(r, 1);
    s->pps[id].valid = 1;
}

static void copy_ref_list_modification(reader *r, writer *w)
{
    if (!cu(r, w, 1))
        return;
    for (int i = 0; i < 64; i++) {
        uint32_t idc = cue(r, w);
        if (idc == 3)
            break;
        cue(r, w);
    }
}

static void copy_weights(reader *r, writer *w, const h264p_state *s, int count)
{
    for (int i = 0; i < count; i++) {
        if (cu(r, w, 1)) {
            cse(r, w);
            cse(r, w);
        }
        if (s->chroma_format_idc != 0 && cu(r, w, 1))
            for (int j = 0; j < 4; j++)
                cse(r, w);
    }
}

static int rewrite_slice(h264p_state *s, reader *r, writer *w, int nal_type, int nal_ref_idc)
{
    uint32_t pps_id;
    int slice_type, l0, l1;

    cue(r, w);
    slice_type = (int)(cue(r, w) % 5);   /* 0 P, 1 B, 2 I, 3 SP, 4 SI */
    pps_id = cue(r, w);
    if (pps_id >= H264P_MAX_PPS || !s->pps[pps_id].valid || slice_type >= 3)
        return H264P_ERROR;
    if (s->separate_colour_plane)
        cu(r, w, 2);
    cu(r, w, s->log2_max_frame_num);
    if (!s->frame_mbs_only && rd(r, 1))
        return H264P_FIELD;              /* field_pic_flag: dropped from the output when 0 */
    if (nal_type == NAL_IDR)
        cue(r, w);
    if (s->poc_type == 0) {
        cu(r, w, s->log2_max_poc_lsb);
        if (s->pps[pps_id].bottom_field_pic_order)
            cse(r, w);
    } else if (s->poc_type == 1 && !s->delta_pic_order_always_zero) {
        cse(r, w);
        if (s->pps[pps_id].bottom_field_pic_order)
            cse(r, w);
    }
    if (s->pps[pps_id].redundant_pic_cnt)
        cue(r, w);
    if (slice_type == 1)
        cu(r, w, 1);
    l0 = s->pps[pps_id].num_ref_idx_l0;
    l1 = s->pps[pps_id].num_ref_idx_l1;
    if (slice_type == 0 || slice_type == 1) {
        if (cu(r, w, 1)) {
            l0 = (int)cue(r, w) + 1;
            if (slice_type == 1)
                l1 = (int)cue(r, w) + 1;
        }
        copy_ref_list_modification(r, w);
        if (slice_type == 1)
            copy_ref_list_modification(r, w);
    }
    if ((s->pps[pps_id].weighted_pred && slice_type == 0) || (s->pps[pps_id].weighted_bipred_idc == 1 && slice_type == 1)) {
        cue(r, w);
        if (s->chroma_format_idc != 0)
            cue(r, w);
        copy_weights(r, w, s, l0);
        if (slice_type == 1)
            copy_weights(r, w, s, l1);
    }
    if (nal_ref_idc) {
        if (nal_type == NAL_IDR) {
            cu(r, w, 2);
        } else if (cu(r, w, 1)) {
            for (int i = 0; i < 64; i++) {
                uint32_t op = cue(r, w);
                if (op == 0)
                    break;
                if (op == 1 || op == 3)
                    cue(r, w);
                if (op == 2)
                    cue(r, w);
                if (op == 3 || op == 6)
                    cue(r, w);
                if (op == 4)
                    cue(r, w);
            }
        }
    }
    if (s->pps[pps_id].cabac && slice_type != 2)
        cue(r, w);
    cse(r, w);
    if (s->pps[pps_id].deblocking_control && cue(r, w) != 1) {
        cse(r, w);
        cse(r, w);
    }
    if (s->pps[pps_id].cabac) {
        /* cabac_alignment_one_bit, then byte-aligned slice data copied as bytes. */
        while (r->pos & 7)
            rd(r, 1);
        while (w->pos & 7)
            wr(w, 1, 1);
        while (r->pos < r->bits)
            cu(r, w, 8);
    } else {
        copy_tail(r, w);
    }
    return H264P_REWRITTEN;
}

int h264p_rewrite(h264p_state *s, const uint8_t *nal, uint32_t size, uint8_t *out, uint32_t *out_size)
{
    uint8_t *rbsp, *wbuf;
    uint32_t n = 0, zeros = 0, i, o;
    int type, ret;
    reader r;
    writer w;

    if (size < 2)
        return H264P_COPY;
    type = nal[0] & 0x1F;
    if (type != NAL_SPS && type != NAL_PPS && type != NAL_SLICE && type != NAL_IDR)
        return H264P_COPY;
    if ((type == NAL_SLICE || type == NAL_IDR) && (!s->have_sps || s->frame_mbs_only))
        return H264P_COPY;
    rbsp = malloc(size);
    wbuf = calloc(1, size + 16);
    if (!rbsp || !wbuf) {
        free(rbsp);
        free(wbuf);
        return H264P_ERROR;
    }
    for (i = 1; i < size; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        rbsp[n++] = nal[i];
    }
    r.buf = rbsp;
    r.bits = n * 8;
    r.pos = 0;
    w.buf = wbuf;
    w.cap_bits = (size + 16) * 8;
    w.pos = 0;
    w.overflow = 0;

    if (type == NAL_PPS) {
        parse_pps(s, &r);
        ret = H264P_COPY;
    } else if (type == NAL_SPS) {
        ret = rewrite_sps(s, &r, &w);
        if (ret != H264P_REWRITTEN)
            s->have_sps = 0;
    } else {
        ret = rewrite_slice(s, &r, &w, type, (nal[0] >> 5) & 3);
    }
    if (ret == H264P_REWRITTEN && w.overflow)
        ret = H264P_ERROR;
    if (ret == H264P_REWRITTEN) {
        /* Header byte, then the payload with emulation prevention bytes put back. */
        out[0] = nal[0];
        o = 1;
        zeros = 0;
        for (i = 0; i < w.pos / 8; i++) {
            if (zeros >= 2 && wbuf[i] <= 3) {
                out[o++] = 3;
                zeros = 0;
            }
            out[o++] = wbuf[i];
            zeros = wbuf[i] == 0 ? zeros + 1 : 0;
        }
        if (o > 1 && out[o - 1] == 0)
            out[o++] = 3;
        *out_size = o;
    }
    free(rbsp);
    free(wbuf);
    return ret;
}
