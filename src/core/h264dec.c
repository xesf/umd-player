/* H.264 decoder: NAL units, parameter sets, slice headers, picture order count, the decoded
   picture buffer, reference picture lists and output order. The macroblock layer is in
   h264dec_slice.c. Section numbers refer to ITU-T H.264. */
#include "h264dec_int.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bit reader over an RBSP ---- */

typedef struct bits {
    const uint8_t *p;
    uint32_t size, pos;            /* in bits */
} bits;

static uint32_t u(bits *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        uint32_t bit = b->pos < b->size ? (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1 : 0;
        v = (v << 1) | bit;
        b->pos++;
    }
    return v;
}

static uint32_t ue(bits *b)
{
    int zeros = 0;
    while (b->pos < b->size && u(b, 1) == 0 && zeros < 32)
        zeros++;
    return zeros ? ((1u << zeros) - 1) + u(b, zeros) : 0;
}

static int32_t se(bits *b)
{
    uint32_t k = ue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

static int more_rbsp_data(const bits *b)
{
    /* there is data before the rbsp_stop_one_bit */
    uint32_t last = b->size;
    while (last > b->pos && !((b->p[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1))
        last--;
    return last > b->pos + 1;
}

/* Copies a NAL unit's payload without emulation prevention bytes. */
static uint32_t to_rbsp(h264dec *d, const uint8_t *nal, uint32_t size)
{
    uint32_t n = 0, zeros = 0;

    if (size > d->rbsp_cap) {
        free(d->rbsp);
        d->rbsp_cap = size + 64;
        d->rbsp = malloc(d->rbsp_cap);
        if (!d->rbsp) {
            d->rbsp_cap = 0;
            return 0;
        }
    }
    for (uint32_t i = 0; i < size; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        d->rbsp[n++] = nal[i];
    }
    memset(d->rbsp + n, 0, d->rbsp_cap - n < 8 ? d->rbsp_cap - n : 8);
    return n;
}

int h264dec_fail(h264dec *d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->err, sizeof(d->err), fmt, ap);
    va_end(ap);
    return -1;
}

const char *h264dec_error(const h264dec *d)
{
    return d->err;
}

/* ---- parameter sets, 7.3.2.1 and 7.3.2.2 ---- */

static void skip_hrd(bits *b)
{
    int cpb_cnt = (int)ue(b) + 1;
    u(b, 4);
    u(b, 4);
    for (int i = 0; i < cpb_cnt && i < 32; i++) {
        ue(b);
        ue(b);
        u(b, 1);
    }
    u(b, 20);                      /* four 5-bit lengths */
}

static int parse_sps(h264dec *d, bits *b)
{
    dec_sps s;
    int id;

    memset(&s, 0, sizeof(s));
    s.profile_idc = (int)u(b, 8);
    u(b, 16);                      /* constraint flags, reserved bits, level_idc below */
    s.level_idc = (int)(b->p[2]);
    id = (int)ue(b);
    if (id >= MAX_SPS)
        return h264dec_fail(d, "SPS id %d", id);
    s.chroma_format_idc = 1;
    if (s.profile_idc == 100 || s.profile_idc == 110 || s.profile_idc == 122 || s.profile_idc == 144) {
        s.chroma_format_idc = (int)ue(b);
        if (s.chroma_format_idc == 3)
            u(b, 1);
        if (ue(b) != 0 || ue(b) != 0)
            return h264dec_fail(d, "bit depth above 8");
        u(b, 1);
        if (u(b, 1))
            return h264dec_fail(d, "scaling matrices are not supported");
    }
    if (s.chroma_format_idc != 1)
        return h264dec_fail(d, "chroma format %d is not supported", s.chroma_format_idc);
    s.log2_max_frame_num = (int)ue(b) + 4;
    s.poc_type = (int)ue(b);
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = (int)ue(b) + 4;
    } else if (s.poc_type == 1) {
        return h264dec_fail(d, "picture order count type 1 is not supported");
    }
    s.max_num_ref_frames = (int)ue(b);
    u(b, 1);                       /* gaps_in_frame_num_value_allowed_flag */
    s.mb_w = (int)ue(b) + 1;
    s.mb_h = (int)ue(b) + 1;
    s.frame_mbs_only = (int)u(b, 1);
    if (!s.frame_mbs_only)
        s.mb_h *= 2;
    if (!s.frame_mbs_only)
        s.mbaff = (int)u(b, 1);
    s.direct_8x8_inference = (int)u(b, 1);
    if (u(b, 1)) {
        int cy = s.frame_mbs_only ? 2 : 4;
        s.crop_left = 2 * (int)ue(b);
        s.crop_right = 2 * (int)ue(b);
        s.crop_top = cy * (int)ue(b);
        s.crop_bottom = cy * (int)ue(b);
    }
    s.num_reorder_frames = -1;
    if (u(b, 1)) {                 /* VUI, E.1.1 */
        int nal_hrd, vcl_hrd;
        if (u(b, 1) && u(b, 8) == 255)
            u(b, 32);
        if (u(b, 1))
            u(b, 1);
        if (u(b, 1)) {
            u(b, 4);
            if (u(b, 1))
                u(b, 24);
        }
        if (u(b, 1)) {
            ue(b);
            ue(b);
        }
        if (u(b, 1))
            u(b, 32 + 32 + 1);
        nal_hrd = (int)u(b, 1);
        if (nal_hrd)
            skip_hrd(b);
        vcl_hrd = (int)u(b, 1);
        if (vcl_hrd)
            skip_hrd(b);
        if (nal_hrd || vcl_hrd)
            u(b, 1);
        u(b, 1);                   /* pic_struct_present_flag */
        if (u(b, 1)) {             /* bitstream_restriction_flag */
            u(b, 1);
            ue(b);
            ue(b);
            ue(b);
            ue(b);
            s.num_reorder_frames = (int)ue(b);
            ue(b);
        }
    }
    if (s.mb_w > 256 || s.mb_h > 256)
        return h264dec_fail(d, "picture too large");
    s.valid = 1;
    d->sps[id] = s;
    return 0;
}

static int parse_pps(h264dec *d, bits *b)
{
    dec_pps p;
    int id = (int)ue(b);

    memset(&p, 0, sizeof(p));
    if (id >= MAX_PPS)
        return h264dec_fail(d, "PPS id %d", id);
    p.sps_id = (int)ue(b);
    if (p.sps_id >= MAX_SPS)
        return h264dec_fail(d, "PPS refers to SPS %d", p.sps_id);
    p.cabac = (int)u(b, 1);
    p.bottom_field_pic_order_in_frame_present = (int)u(b, 1);
    if (ue(b) != 0)
        return h264dec_fail(d, "slice groups are not supported");
    p.num_ref_idx_default[0] = (int)ue(b) + 1;
    p.num_ref_idx_default[1] = (int)ue(b) + 1;
    p.weighted_pred = (int)u(b, 1);
    p.weighted_bipred_idc = (int)u(b, 2);
    p.pic_init_qp = 26 + se(b);
    se(b);                         /* pic_init_qs */
    p.chroma_qp_index_offset = se(b);
    p.deblocking_filter_control_present = (int)u(b, 1);
    p.constrained_intra_pred = (int)u(b, 1);
    p.redundant_pic_cnt_present = (int)u(b, 1);
    if (more_rbsp_data(b)) {
        if (u(b, 1))
            return h264dec_fail(d, "8x8 transforms are not supported");
        if (u(b, 1))
            return h264dec_fail(d, "scaling matrices are not supported");
        if (se(b) != p.chroma_qp_index_offset)
            return h264dec_fail(d, "a second chroma QP offset is not supported");
    }
    p.valid = 1;
    d->pps[id] = p;
    return 0;
}

/* ---- slice header, 7.3.3 ---- */

typedef struct list_mod {
    int count;
    int op[MAX_LIST + 1], val[MAX_LIST + 1];
} list_mod;

static int parse_pred_weights(h264dec *d, bits *b, dec_slice *s)
{
    s->luma_log2_wd = (int)ue(b);
    s->chroma_log2_wd = (int)ue(b);
    if (s->luma_log2_wd > 7 || s->chroma_log2_wd > 7)
        return h264dec_fail(d, "weight denominator out of range");
    for (int l = 0; l < (s->type == SLICE_B ? 2 : 1); l++) {
        for (int i = 0; i < s->num_ref[l]; i++) {
            s->luma_w[l][i] = (int16_t)(1 << s->luma_log2_wd);
            s->luma_o[l][i] = 0;
            s->chroma_w[l][i][0] = s->chroma_w[l][i][1] = (int16_t)(1 << s->chroma_log2_wd);
            s->chroma_o[l][i][0] = s->chroma_o[l][i][1] = 0;
            s->weighted[l][i] = 0;
            if (u(b, 1)) {
                s->luma_w[l][i] = (int16_t)se(b);
                s->luma_o[l][i] = (int16_t)se(b);
                s->weighted[l][i] = 1;
            }
            if (u(b, 1)) {
                for (int c = 0; c < 2; c++) {
                    s->chroma_w[l][i][c] = (int16_t)se(b);
                    s->chroma_o[l][i][c] = (int16_t)se(b);
                }
                s->weighted[l][i] = 1;
            }
        }
    }
    return 0;
}

static int parse_slice_header(h264dec *d, bits *b, int nal_type, int nal_ref_idc, dec_slice *s, list_mod mod[2])
{
    const dec_sps *sps;
    const dec_pps *pps;

    memset(s, 0, offsetof(dec_slice, luma_log2_wd));
    s->nal_ref_idc = nal_ref_idc;
    s->idr = nal_type == 5;
    s->first_mb = (int)ue(b);
    s->type = (int)(ue(b) % 5);
    if (s->type > SLICE_I)
        return h264dec_fail(d, "SP and SI slices are not supported");
    s->pps_id = (int)ue(b);
    if (s->pps_id >= MAX_PPS || !d->pps[s->pps_id].valid)
        return h264dec_fail(d, "slice refers to missing PPS %d", s->pps_id);
    pps = &d->pps[s->pps_id];
    sps = &d->sps[pps->sps_id];
    if (!sps->valid)
        return h264dec_fail(d, "slice refers to missing SPS %d", pps->sps_id);
    if (!pps->cabac)
        return h264dec_fail(d, "CAVLC streams are not supported");
    if (sps->mbaff)
        return h264dec_fail(d, "MBAFF streams are not supported");
    s->frame_num = (int)u(b, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only) {
        s->field_pic = (int)u(b, 1);
        if (s->field_pic)
            s->bottom_field = (int)u(b, 1);
    }
    if (s->idr)
        s->idr_pic_id = (int)ue(b);
    if (sps->poc_type == 0) {
        s->poc_lsb = (int)u(b, sps->log2_max_poc_lsb);
        if (pps->bottom_field_pic_order_in_frame_present && !s->field_pic)
            s->delta_poc_bottom = se(b);
    }
    if (pps->redundant_pic_cnt_present && ue(b) != 0)
        return 1;                  /* a redundant picture: ignored */
    if (s->type == SLICE_B)
        s->direct_spatial = (int)u(b, 1);
    s->num_ref[0] = pps->num_ref_idx_default[0];
    s->num_ref[1] = pps->num_ref_idx_default[1];
    if (s->type != SLICE_I && u(b, 1)) {
        s->num_ref[0] = (int)ue(b) + 1;
        if (s->type == SLICE_B)
            s->num_ref[1] = (int)ue(b) + 1;
    }
    if (s->type == SLICE_I)
        s->num_ref[0] = s->num_ref[1] = 0;
    if (s->type != SLICE_B)
        s->num_ref[1] = 0;
    if (s->num_ref[0] > 32 || s->num_ref[1] > 32)
        return h264dec_fail(d, "too many reference indices");
    for (int l = 0; l < 2; l++) {
        mod[l].count = 0;
        if (l < (s->type == SLICE_B ? 2 : s->type == SLICE_P ? 1 : 0) && u(b, 1)) {
            for (;;) {
                int op = (int)ue(b);
                if (op == 3)
                    break;
                if (op > 2 || mod[l].count > MAX_LIST)
                    return h264dec_fail(d, "bad reference list modification");
                if (op == 2)
                    return h264dec_fail(d, "long-term references are not supported");
                mod[l].op[mod[l].count] = op;
                mod[l].val[mod[l].count] = (int)ue(b);
                mod[l].count++;
            }
        }
    }
    if ((pps->weighted_pred && s->type == SLICE_P) || (pps->weighted_bipred_idc == 1 && s->type == SLICE_B)) {
        if (parse_pred_weights(d, b, s) < 0)
            return -1;
    } else {
        memset(s->weighted, 0, sizeof(s->weighted));
    }
    if (s->type == SLICE_B && pps->weighted_bipred_idc == 2)
        return h264dec_fail(d, "implicit weighted prediction is not supported");
    if (nal_ref_idc) {
        if (s->idr) {
            u(b, 1);               /* no_output_of_prior_pics_flag */
            if (u(b, 1))
                return h264dec_fail(d, "long-term references are not supported");
        } else if (u(b, 1)) {
            return h264dec_fail(d, "memory management control operations are not supported");
        }
    }
    if (s->type != SLICE_I) {
        s->cabac_init_idc = (int)ue(b);
        if (s->cabac_init_idc > 2)
            return h264dec_fail(d, "cabac_init_idc %d", s->cabac_init_idc);
    }
    s->qp = pps->pic_init_qp + se(b);
    if (s->qp < 0 || s->qp > 51)
        return h264dec_fail(d, "slice QP %d", s->qp);
    if (pps->deblocking_filter_control_present) {
        s->disable_deblock = (int)ue(b);
        if (s->disable_deblock != 1) {
            s->alpha_offset = 2 * se(b);
            s->beta_offset = 2 * se(b);
        }
    }
    if (s->type == SLICE_B && !s->direct_spatial)
        return h264dec_fail(d, "temporal direct prediction is not supported");
    return 0;
}

/* ---- the decoded picture buffer ---- */

static int poc_of(const picture *p, int parity)
{
    if (parity == 1)
        return p->poc_top;
    if (parity == 2)
        return p->poc_bot;
    return p->poc_top < p->poc_bot ? p->poc_top : p->poc_bot;
}

int h264dec_ref_poc(const refpic *r)
{
    return poc_of(r->pic, r->parity);
}

static int picture_free(const h264dec *d, const picture *p)
{
    return !p->used || (!p->ref && !p->output_needed && p != d->cur &&
                        p->plane[0] != d->out_frame.plane[0]);
}

static void release_unused(h264dec *d)
{
    for (int i = 0; i < DPB_FRAMES; i++) {
        picture *p = &d->dpb[i];
        if (p->used && !p->ref && !p->output_needed && p != d->cur && p->plane[0] != d->out_frame.plane[0])
            p->used = 0;
    }
}

static int alloc_planes(h264dec *d, picture *p)
{
    int w = d->mb_w * 16, h = d->mb_h * 16;
    if (p->plane[0] && p->stride[0] == w && p->mb)
        return 0;
    free(p->plane[0]);
    free(p->mb);
    p->plane[0] = malloc((size_t)w * h * 3 / 2);
    p->mb = calloc((size_t)d->mb_w * d->mb_h, sizeof(mbinfo));
    if (!p->plane[0] || !p->mb)
        return -1;
    p->plane[1] = p->plane[0] + w * h;
    p->plane[2] = p->plane[1] + w * h / 4;
    p->stride[0] = w;
    p->stride[1] = p->stride[2] = w / 2;
    return 0;
}

/* Frames waiting for output, in display order: earlier IDR periods first, then the smallest
   POC. */
static picture *next_output(h264dec *d)
{
    picture *best = NULL;
    for (int i = 0; i < DPB_FRAMES; i++) {
        picture *p = &d->dpb[i];
        if (p->used && p->output_needed && p != d->cur &&
            (!best || p->epoch < best->epoch || (p->epoch == best->epoch && poc_of(p, 3) < poc_of(best, 3))))
            best = p;
    }
    return best;
}

static int waiting_output(const h264dec *d)
{
    int n = 0;
    for (int i = 0; i < DPB_FRAMES; i++)
        n += d->dpb[i].used && d->dpb[i].output_needed && &d->dpb[i] != d->cur;
    return n;
}

static picture *new_picture(h264dec *d)
{
    release_unused(d);
    for (int i = 0; i < DPB_FRAMES; i++) {
        picture *p = &d->dpb[i];
        if (picture_free(d, p) && !p->used) {
            if (alloc_planes(d, p) < 0)
                return NULL;
            p->used = 1;
            p->fields = p->ref = 0;
            p->output_needed = 0;
            p->coded_frame = 0;
            p->id = ++d->next_pic_id;
            return p;
        }
    }
    return NULL;
}

/* ---- picture order count, 8.2.1.1 ---- */

static void compute_poc(h264dec *d)
{
    const dec_sps *sps = d->cur_sps;
    const dec_slice *s = &d->sh;
    int max_lsb = 1 << sps->log2_max_poc_lsb, msb;

    if (sps->poc_type == 2) {      /* 8.2.1.3: output order is decoding order */
        int offset = 0, abs;
        (void)offset;
        abs = s->frame_num;        /* within a run without wrap; enough for display order */
        d->poc = 2 * abs - (s->nal_ref_idc == 0);
        if (s->idr)
            d->poc = 0;
        d->cur->poc_top = d->cur->poc_bot = d->poc;
        return;
    }
    if (s->idr) {
        d->prev_poc_msb = 0;
        d->prev_poc_lsb = 0;
    }
    if (s->poc_lsb < d->prev_poc_lsb && d->prev_poc_lsb - s->poc_lsb >= max_lsb / 2)
        msb = d->prev_poc_msb + max_lsb;
    else if (s->poc_lsb > d->prev_poc_lsb && s->poc_lsb - d->prev_poc_lsb > max_lsb / 2)
        msb = d->prev_poc_msb - max_lsb;
    else
        msb = d->prev_poc_msb;
    if (d->structure == 3) {
        d->cur->poc_top = msb + s->poc_lsb;
        d->cur->poc_bot = d->cur->poc_top + s->delta_poc_bottom;
        d->poc = d->cur->poc_top < d->cur->poc_bot ? d->cur->poc_top : d->cur->poc_bot;
    } else if (d->structure == 1) {
        d->cur->poc_top = d->poc = msb + s->poc_lsb;
    } else {
        d->cur->poc_bot = d->poc = msb + s->poc_lsb;
    }
    if (s->nal_ref_idc) {
        d->prev_poc_msb = msb;
        d->prev_poc_lsb = s->poc_lsb;
    }
}

/* ---- reference picture lists, 8.2.4 ---- */

static int frame_num_wrap(const h264dec *d, const picture *p)
{
    return p->frame_num > d->sh.frame_num ? p->frame_num - d->max_frame_num : p->frame_num;
}

/* The short-term reference frames this picture may use, with the fields that are references. */
static int reference_frames(h264dec *d, picture **out)
{
    int n = 0;
    for (int i = 0; i < DPB_FRAMES; i++) {
        picture *p = &d->dpb[i];
        if (!p->used || !p->ref)
            continue;
        if (p == d->cur && d->structure == 3)
            continue;
        if (d->structure == 3 && p->ref != 3)
            continue;              /* frames use frames whose two fields are references */
        p->frame_num_wrap = frame_num_wrap(d, p);
        out[n++] = p;
    }
    return n;
}

/* PicOrderCnt of a reference entry while decoding a field: its reference fields only. */
static int entry_poc(const h264dec *d, const picture *p)
{
    int ref = p->ref;
    if (p == d->cur)
        ref &= 3 - d->structure;   /* the first field of the current frame */
    if (ref == 1)
        return p->poc_top;
    if (ref == 2)
        return p->poc_bot;
    return p->poc_top < p->poc_bot ? p->poc_top : p->poc_bot;
}

/* 8.2.4.2.5: fields alternating in parity, starting with the current field's. */
static int split_fields(h264dec *d, picture **frames, int n, refpic *list)
{
    int len = 0, idx[2] = {0, 0}, parity = d->structure, which = 0;
    for (;;) {
        int want = which == 0 ? parity : 3 - parity, *k = &idx[which];
        while (*k < n && !(frames[*k]->ref & want & (frames[*k] == d->cur ? 3 - d->structure : 3)))
            (*k)++;
        if (*k < n) {
            list[len].pic = frames[*k];
            list[len].parity = want;
            len++;
            (*k)++;
            which ^= 1;
            continue;
        }
        /* this parity ran out: the rest of the other parity in order */
        {
            int other = which ^ 1, *o = &idx[other], ow = other == 0 ? parity : 3 - parity;
            for (; *o < n; (*o)++)
                if (frames[*o]->ref & ow & (frames[*o] == d->cur ? 3 - d->structure : 3)) {
                    list[len].pic = frames[*o];
                    list[len].parity = ow;
                    len++;
                }
        }
        break;
    }
    return len;
}

static void sort_frames(picture **f, int n, int (*key)(const h264dec *, const picture *), const h264dec *d, int descending)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            int a = key(d, f[j - 1]), b = key(d, f[j]);
            if (descending ? a < b : a > b) {
                picture *t = f[j - 1];
                f[j - 1] = f[j];
                f[j] = t;
            } else {
                break;
            }
        }
}

static int key_wrap(const h264dec *d, const picture *p) { (void)d; return p->frame_num_wrap; }
static int key_poc(const h264dec *d, const picture *p) { return d->structure == 3 ? poc_of(p, 3) : entry_poc(d, p); }

/* The B list ordering: entries before the current picture (descending), then after
   (ascending), or the reverse for list 1. */
static int b_order(h264dec *d, picture **frames, int n, int list1, picture **out)
{
    picture *before[DPB_FRAMES], *after[DPB_FRAMES];
    int nb = 0, na = 0, k = 0, cur = d->poc;
    for (int i = 0; i < n; i++) {
        int poc = key_poc(d, frames[i]);
        if (d->structure == 3 ? poc < cur : poc <= cur)
            before[nb++] = frames[i];
        else
            after[na++] = frames[i];
    }
    sort_frames(before, nb, key_poc, d, 1);
    sort_frames(after, na, key_poc, d, 0);
    if (!list1) {
        for (int i = 0; i < nb; i++) out[k++] = before[i];
        for (int i = 0; i < na; i++) out[k++] = after[i];
    } else {
        for (int i = 0; i < na; i++) out[k++] = after[i];
        for (int i = 0; i < nb; i++) out[k++] = before[i];
    }
    return k;
}

static int pic_num(const h264dec *d, const refpic *r)
{
    int wrap = frame_num_wrap(d, r->pic);
    if (d->structure == 3)
        return wrap;
    return r->parity == d->structure ? 2 * wrap + 1 : 2 * wrap;
}

static int apply_modification(h264dec *d, int l, const list_mod *m)
{
    int cur_num = d->structure == 3 ? d->sh.frame_num : 2 * d->sh.frame_num + 1;
    int max_num = d->structure == 3 ? d->max_frame_num : 2 * d->max_frame_num;
    int pred = cur_num, n = d->sh.num_ref[l];
    refpic *list = d->list[l];

    for (int i = 0; i < m->count; i++) {
        int diff = m->val[i] + 1, no_wrap, num, found = -1, k;
        refpic r;
        if (m->op[i] == 0)
            no_wrap = pred - diff < 0 ? pred - diff + max_num : pred - diff;
        else
            no_wrap = pred + diff >= max_num ? pred + diff - max_num : pred + diff;
        pred = no_wrap;
        num = no_wrap > cur_num ? no_wrap - max_num : no_wrap;
        /* find the short-term picture with this PicNum */
        for (int j = 0; j < DPB_FRAMES && found < 0; j++) {
            picture *p = &d->dpb[j];
            int parities = d->structure == 3 ? 3 : 0;
            if (!p->used || !p->ref || (p == d->cur && d->structure == 3))
                continue;
            if (d->structure == 3) {
                if (p->ref != 3)
                    continue;
                r.pic = p;
                r.parity = 3;
                if (pic_num(d, &r) == num)
                    found = j;
            } else {
                for (int par = 1; par <= 2 && found < 0; par++) {
                    int avail = p->ref & (p == d->cur ? 3 - d->structure : 3);
                    if (!(avail & par))
                        continue;
                    r.pic = p;
                    r.parity = par;
                    if (pic_num(d, &r) == num) {
                        found = j;
                        parities = par;
                    }
                }
            }
            if (found >= 0)
                r.parity = parities;
        }
        if (found < 0)
            return h264dec_fail(d, "reference list modification names a missing picture");
        r.pic = &d->dpb[found];
        /* 8.2.4.3.1: insert at index i, then drop the later duplicate */
        for (k = n; k > i; k--)
            list[k] = list[k - 1];
        list[i] = r;
        {
            int w = i + 1;
            for (k = i + 1; k <= n; k++)
                if (!(list[k].pic == r.pic && list[k].parity == r.parity) || !list[k].pic)
                    list[w++] = list[k];
        }
    }
    return 0;
}

static int build_lists(h264dec *d, const list_mod mod[2])
{
    picture *frames[DPB_FRAMES], *ordered[DPB_FRAMES];
    int n = reference_frames(d, frames), len[2] = {0, 0};

    memset(d->list, 0, sizeof(d->list));
    if (d->sh.type == SLICE_P) {
        sort_frames(frames, n, key_wrap, d, 1);
        if (d->structure == 3) {
            for (int i = 0; i < n; i++) {
                d->list[0][i].pic = frames[i];
                d->list[0][i].parity = 3;
            }
            len[0] = n;
        } else {
            len[0] = split_fields(d, frames, n, d->list[0]);
        }
    } else if (d->sh.type == SLICE_B) {
        for (int l = 0; l < 2; l++) {
            int k = b_order(d, frames, n, l, ordered);
            if (d->structure == 3) {
                for (int i = 0; i < k; i++) {
                    d->list[l][i].pic = ordered[i];
                    d->list[l][i].parity = 3;
                }
                len[l] = k;
            } else {
                len[l] = split_fields(d, ordered, k, d->list[l]);
            }
        }
        if (len[1] > 1 && len[0] == len[1]) {
            int same = 1;
            for (int i = 0; i < len[0]; i++)
                same &= d->list[0][i].pic == d->list[1][i].pic && d->list[0][i].parity == d->list[1][i].parity;
            if (same) {
                refpic t = d->list[1][0];
                d->list[1][0] = d->list[1][1];
                d->list[1][1] = t;
            }
        }
    }
    for (int l = 0; l < 2; l++) {
        for (int i = d->sh.num_ref[l]; i < MAX_LIST; i++)
            d->list[l][i].pic = NULL;
        if (mod[l].count && apply_modification(d, l, &mod[l]) < 0)
            return -1;
        d->list_len[l] = d->sh.num_ref[l];
        /* Entries without a picture: after a seek the stream can name more references than
           have been decoded. They stand in with the first one; a picture with none at all
           cannot be decoded. */
        for (int i = 0; i < d->sh.num_ref[l]; i++)
            if (!d->list[l][i].pic) {
                int k = 0;
                while (k < d->sh.num_ref[l] && !d->list[l][k].pic)
                    k++;
                if (k == d->sh.num_ref[l])
                    return h264dec_fail(d, "reference list %d is empty (%d references)", l, len[l]);
                d->list[l][i] = d->list[l][k];
            }
    }
    return 0;
}

/* ---- pictures ---- */

static void finish_picture(h264dec *d)
{
    picture *p = d->cur;

    if (!d->decoding)
        return;
    d->decoding = 0;
    if (!getenv("H264DEC_NODEBLOCK"))
        h264dec_deblock_picture(d);
    p->fields |= d->structure;
    if (d->sh.nal_ref_idc) {
        int second_of_ref_pair = d->structure != 3 && (p->ref & (3 - d->structure));
        if (!d->sh.idr && !second_of_ref_pair) {
            /* 8.2.5.3: the sliding window */
            int count = 0;
            for (int i = 0; i < DPB_FRAMES; i++)
                count += d->dpb[i].used && d->dpb[i].ref && &d->dpb[i] != p;
            while (count >= (d->cur_sps->max_num_ref_frames > 0 ? d->cur_sps->max_num_ref_frames : 1)) {
                picture *oldest = NULL;
                for (int i = 0; i < DPB_FRAMES; i++) {
                    picture *q = &d->dpb[i];
                    if (q->used && q->ref && q != p) {
                        q->frame_num_wrap = frame_num_wrap(d, q);
                        if (!oldest || q->frame_num_wrap < oldest->frame_num_wrap)
                            oldest = q;
                    }
                }
                if (!oldest)
                    break;
                oldest->ref = 0;
                count--;
            }
        }
        p->ref |= d->structure;
        d->prev_ref_frame_num = d->sh.frame_num;
    }
    if (p->fields == 3)
        p->output_needed = 1;
    d->cur = p->fields == 3 ? NULL : p;   /* a lone first field waits for its pair */
}

static int start_picture(h264dec *d, int nal_ref_idc)
{
    dec_slice *s = &d->sh;
    const dec_sps *sps = d->cur_sps;
    picture *p = NULL;

    if (sps->mb_w != d->mb_w || sps->mb_h != d->mb_h) {
        /* a new size: start over (this picture is the I picture that decoding restarts at) */
        h264dec_reset(d);
        d->need_key = 0;
        d->mb_w = sps->mb_w;
        d->mb_h = sps->mb_h;
        for (int i = 0; i < DPB_FRAMES; i++) {
            free(d->dpb[i].plane[0]);
            free(d->dpb[i].mb);
            d->dpb[i].plane[0] = NULL;
            d->dpb[i].mb = NULL;
        }
        free(d->ctx);
        d->ctx = calloc((size_t)d->mb_w * d->mb_h, sizeof(mbctx));
        if (!d->ctx)
            return h264dec_fail(d, "out of memory");
    }
    d->max_frame_num = 1 << sps->log2_max_frame_num;
    d->structure = s->field_pic ? (s->bottom_field ? 2 : 1) : 3;
    d->num_reorder = sps->num_reorder_frames >= 0 ? sps->num_reorder_frames : sps->max_num_ref_frames;
    if (d->num_reorder > 16)
        d->num_reorder = 16;

    /* the second field of the pending frame? */
    if (d->cur && d->structure != 3 && !(d->cur->fields & d->structure) && d->cur->frame_num == s->frame_num) {
        p = d->cur;
    } else {
        if (s->idr) {
            /* no reference survives an IDR picture, and everything before it is shown first */
            for (int i = 0; i < DPB_FRAMES; i++)
                if (&d->dpb[i] != d->cur)
                    d->dpb[i].ref = 0;
            d->epoch++;
        }
        if (d->cur) {
            /* an unpaired field: show it as it is */
            d->cur->output_needed = 1;
            d->cur = NULL;
        }
        p = new_picture(d);
        while (!p) {
            picture *o = next_output(d);
            if (!o)
                return h264dec_fail(d, "no free picture buffer");
            o->output_needed = 0;  /* dropped: the caller is not reading output */
            p = new_picture(d);
        }
        p->frame_num = s->frame_num;
        p->coded_frame = d->structure == 3;
        p->pts = d->pts;
        p->top_field_first = d->structure != 2;
        p->poc_top = p->poc_bot = 0;
        p->epoch = d->epoch;
    }
    d->cur = p;
    compute_poc(d);
    (void)nal_ref_idc;
    if (getenv("H264DEC_TRACE"))
        fprintf(stderr, "picture %s%s frame_num %d poc %d ref %d structure %d\n", &"PBI"[s->type] ? (s->type == 0 ? "P" : s->type == 1 ? "B" : "I") : "", s->idr ? " IDR" : "",
                s->frame_num, d->poc, s->nal_ref_idc, d->structure);

    /* where this picture's samples and macroblocks go */
    {
        int bottom = d->structure == 2, field = d->structure != 3;
        for (int c = 0; c < 3; c++) {
            d->dst[c] = p->plane[c] + (bottom ? p->stride[c] : 0);
            d->dst_stride[c] = p->stride[c] << field;
        }
        d->pic_mb_h = field ? d->mb_h / 2 : d->mb_h;
        d->mbi = p->mb + (bottom ? d->mb_w * d->pic_mb_h : 0);
    }
    memset(d->ctx, 0, (size_t)d->mb_w * d->mb_h * sizeof(mbctx));
    d->slice_num = 0;
    d->decoding = 1;
    return 0;
}

/* ---- NAL units ---- */

static int decode_slice(h264dec *d, const uint8_t *nal, uint32_t size)
{
    int nal_ref_idc = (nal[0] >> 5) & 3, nal_type = nal[0] & 0x1F, r;
    uint32_t n = to_rbsp(d, nal + 1, size - 1);
    bits b = {d->rbsp, n * 8, 0};
    list_mod mod[2];
    dec_slice s;

    s = d->sh;
    r = parse_slice_header(d, &b, nal_type, nal_ref_idc, &s, mod);
    if (getenv("H264DEC_TRACE"))
        fprintf(stderr, "slice type %d first_mb %d frame_num %d header %d need_key %d %s\n", s.type, s.first_mb, s.frame_num, r,
                d->need_key, r < 0 ? d->err : "");
    if (r)
        return r < 0 ? -1 : 0;
    if (d->need_key) {
        if (s.type != SLICE_I)
            return 0;
        d->need_key = 0;
        d->recovering = 2;         /* set the point once the picture's POC is known */
    }
    d->sh = s;
    d->cur_pps = &d->pps[s.pps_id];
    d->cur_sps = &d->sps[d->cur_pps->sps_id];
    if (s.first_mb == 0) {
        finish_picture(d);
        if (start_picture(d, nal_ref_idc) < 0)
            return -1;
    } else if (!d->decoding) {
        return 0;                  /* the rest of a picture whose first slice was lost */
    }
    if (s.first_mb == 0 && d->recovering == 2) {
        d->recovering = 1;
        d->recover_epoch = d->epoch;
        d->recover_poc = d->poc;
    } else if (s.first_mb == 0 && d->recovering == 1 && d->epoch == d->recover_epoch && s.type == SLICE_B &&
               d->poc < d->recover_poc) {
        /* shown before the I picture decoding restarted at: its references are gone */
        if (d->cur && d->cur->fields == 0) {
            d->cur->used = 0;
            d->cur = NULL;
        }
        d->decoding = 0;
        return 0;
    } else if (s.first_mb == 0 && d->recovering == 1 && d->epoch != d->recover_epoch) {
        d->recovering = 0;
    }
    if (build_lists(d, mod) < 0) {
        /* a picture referring to pictures we do not have (B pictures after a seek): drop it */
        if (d->cur && d->cur->fields == 0) {
            d->cur->used = 0;
            d->cur = NULL;
        }
        d->decoding = 0;
        return 0;
    }
    d->slice_num++;
    if (getenv("H264DEC_TRACE")) {
        fprintf(stderr, "  lists (mods %d %d):", mod[0].count, mod[1].count);
        for (int l = 0; l < 2; l++)
            for (int i = 0; i < d->list_len[l]; i++)
                fprintf(stderr, " L%d[%d]=fn%d/p%d poc%d", l, i, d->list[l][i].pic->frame_num, d->list[l][i].parity,
                        h264dec_ref_poc(&d->list[l][i]));
        fprintf(stderr, "\n");
    }
    /* CABAC data starts at the next byte boundary (cabac_alignment_one_bit) */
    return h264dec_decode_slice_data(d, d->rbsp, n, (b.pos + 7) & ~7u);
}

int h264dec_decode(h264dec *d, const uint8_t *au, uint32_t size, uint64_t pts)
{
    uint32_t i = 0, start = 0;
    int found = 0;

    d->out_frame.plane[0] = NULL;  /* the frame given out before is no longer read */
    d->pts = pts;
    while (i + 3 <= size) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            if (found) {
                uint32_t end = i;
                while (end > start && au[end - 1] == 0)
                    end--;
                if (end > start) {
                    int t = au[start] & 0x1F, r = 0;
                    if (t == 7 || t == 8) {
                        uint32_t n = to_rbsp(d, au + start + 1, end - start - 1);
                        bits b = {d->rbsp, n * 8, 0};
                        r = t == 7 ? parse_sps(d, &b) : parse_pps(d, &b);
                    } else if (t == 1 || t == 5) {
                        r = decode_slice(d, au + start, end - start);
                    }
                    if (r < 0)
                        return -1;
                }
            }
            i += 3;
            start = i;
            found = 1;
            continue;
        }
        i++;
    }
    if (found && start < size) {
        uint32_t end = size;
        int t = au[start] & 0x1F, r = 0;
        while (end > start && au[end - 1] == 0)
            end--;
        if (end > start) {
            if (t == 7 || t == 8) {
                uint32_t n = to_rbsp(d, au + start + 1, end - start - 1);
                bits b = {d->rbsp, n * 8, 0};
                r = t == 7 ? parse_sps(d, &b) : parse_pps(d, &b);
            } else if (t == 1 || t == 5) {
                r = decode_slice(d, au + start, end - start);
            }
        }
        if (r < 0)
            return -1;
    }
    /* one access unit is one picture: finish it now */
    finish_picture(d);
    return 0;
}

int h264dec_output(h264dec *d, h264dec_frame *out)
{
    picture *p = next_output(d);
    int waiting = waiting_output(d);

    if (!p || (!d->flushing && p->epoch == d->epoch && waiting <= d->num_reorder))
        return 0;
    p->output_needed = 0;
    out->plane[0] = p->plane[0];
    out->plane[1] = p->plane[1];
    out->plane[2] = p->plane[2];
    out->stride[0] = p->stride[0];
    out->stride[1] = p->stride[1];
    out->stride[2] = p->stride[2];
    out->width = d->mb_w * 16 - d->cur_sps->crop_left - d->cur_sps->crop_right;
    out->height = d->mb_h * 16 - d->cur_sps->crop_top - d->cur_sps->crop_bottom;
    out->plane[0] += d->cur_sps->crop_top * p->stride[0] + d->cur_sps->crop_left;
    out->plane[1] += d->cur_sps->crop_top / 2 * p->stride[1] + d->cur_sps->crop_left / 2;
    out->plane[2] += d->cur_sps->crop_top / 2 * p->stride[2] + d->cur_sps->crop_left / 2;
    out->interlaced = !p->coded_frame;
    out->top_field_first = p->top_field_first;
    out->pts = p->pts;
    out->poc = poc_of(p, 3);
    d->out_frame = *out;
    d->out_frame.plane[0] = p->plane[0];   /* remembered to keep the picture until the next call */
    return 1;
}

void h264dec_flush(h264dec *d)
{
    finish_picture(d);
    if (d->cur) {
        d->cur->output_needed = 1;
        d->cur = NULL;
    }
    d->flushing = 1;
}

void h264dec_reset(h264dec *d)
{
    for (int i = 0; i < DPB_FRAMES; i++) {
        d->dpb[i].used = 0;
        d->dpb[i].ref = 0;
        d->dpb[i].output_needed = 0;
    }
    d->cur = NULL;
    d->decoding = 0;
    d->flushing = 0;
    d->out_frame.plane[0] = NULL;
    d->prev_poc_msb = d->prev_poc_lsb = 0;
    d->need_key = 1;
}

h264dec *h264dec_open(void)
{
    h264dec *d = calloc(1, sizeof(h264dec));
    if (d)
        d->need_key = 1;
    return d;
}

void h264dec_close(h264dec *d)
{
    if (!d)
        return;
    for (int i = 0; i < DPB_FRAMES; i++) {
        free(d->dpb[i].plane[0]);
        free(d->dpb[i].mb);
    }
    free(d->ctx);
    free(d->rbsp);
    free(d);
}
