#include "h264.h"

#include <string.h>

#define NAL_SLICE 1
#define NAL_IDR   5
#define NAL_SEI   6
#define NAL_SPS   7
#define NAL_PPS   8
#define SEI_PIC_TIMING 1
#define RBSP_MAX 512   /* headers live at the start of a NAL; this is enough for all of them */

typedef struct {
    uint8_t buf[RBSP_MAX];
    uint32_t size, pos;   /* pos in bits */
} bits;

/* Copies the start of a NAL payload (after its header byte) and removes emulation prevention bytes. */
static void rbsp_load(bits *b, const uint8_t *nal, uint32_t size)
{
    uint32_t i, zeros = 0;

    b->size = 0;
    b->pos = 0;
    for (i = 1; i < size && b->size < RBSP_MAX; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        b->buf[b->size++] = nal[i];
    }
}

static uint32_t get_u(bits *b, int n)
{
    uint32_t v = 0;
    while (n-- > 0) {
        uint32_t byte = b->pos >> 3;
        uint32_t bit = byte < b->size ? (b->buf[byte] >> (7 - (b->pos & 7))) & 1 : 0;
        v = (v << 1) | bit;
        b->pos++;
    }
    return v;
}

static uint32_t get_ue(bits *b)
{
    int zeros = 0;
    while (get_u(b, 1) == 0 && zeros < 32)
        zeros++;
    return zeros ? ((1u << zeros) - 1) + get_u(b, zeros) : 0;
}

static int32_t get_se(bits *b)
{
    uint32_t v = get_ue(b);
    return v & 1 ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
}

static void skip_scaling_list(bits *b, int size)
{
    int last = 8, next = 8, j;
    for (j = 0; j < size; j++) {
        if (next != 0)
            next = (last + get_se(b) + 256) % 256;
        last = next == 0 ? last : next;
    }
}

static void parse_hrd(bits *b, h264_sps *s)
{
    uint32_t count = get_ue(b) + 1, i;

    get_u(b, 4);
    get_u(b, 4);
    for (i = 0; i < count && i < 32; i++) {
        get_ue(b);
        get_ue(b);
        get_u(b, 1);
    }
    get_u(b, 5);
    s->cpb_removal_delay_len = (int)get_u(b, 5) + 1;
    s->dpb_output_delay_len = (int)get_u(b, 5) + 1;
    s->time_offset_len = (int)get_u(b, 5);
}

static void parse_vui(bits *b, h264_sps *s)
{
    static const uint8_t sar_table[17][2] = {
        {0, 0}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11}, {32, 11},
        {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, {4, 3}, {3, 2}, {2, 1}};
    int nal_hrd, vcl_hrd;

    if (get_u(b, 1)) {
        uint32_t idc = get_u(b, 8);
        if (idc == 255) {
            s->sar_w = (int)get_u(b, 16);
            s->sar_h = (int)get_u(b, 16);
        } else if (idc < 17) {
            s->sar_w = sar_table[idc][0];
            s->sar_h = sar_table[idc][1];
        }
    }
    if (get_u(b, 1))
        get_u(b, 1);
    if (get_u(b, 1)) {
        get_u(b, 4);
        if (get_u(b, 1))
            get_u(b, 24);
    }
    if (get_u(b, 1)) {
        get_ue(b);
        get_ue(b);
    }
    if (get_u(b, 1)) {
        s->num_units_in_tick = get_u(b, 32);
        s->time_scale = get_u(b, 32);
        get_u(b, 1);
    }
    nal_hrd = (int)get_u(b, 1);
    if (nal_hrd)
        parse_hrd(b, s);
    vcl_hrd = (int)get_u(b, 1);
    if (vcl_hrd)
        parse_hrd(b, s);
    if (nal_hrd || vcl_hrd)
        get_u(b, 1);
    s->cpb_dpb_delays_present = nal_hrd || vcl_hrd;
    s->pic_struct_present = (int)get_u(b, 1);
}

static void parse_sps(bits *b, h264_sps *s)
{
    uint32_t w_mbs, h_units, i;
    int chroma = 1;

    memset(s, 0, sizeof(*s));
    s->profile_idc = (int)get_u(b, 8);
    get_u(b, 8);
    s->level_idc = (int)get_u(b, 8);
    get_ue(b);
    switch (s->profile_idc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
        chroma = (int)get_ue(b);
        if (chroma == 3)
            get_u(b, 1);
        get_ue(b);
        get_ue(b);
        get_u(b, 1);
        if (get_u(b, 1))
            for (i = 0; i < (chroma != 3 ? 8u : 12u); i++)
                if (get_u(b, 1))
                    skip_scaling_list(b, i < 6 ? 16 : 64);
        break;
    default:
        break;
    }
    s->log2_max_frame_num = (int)get_ue(b) + 4;
    s->poc_type = (int)get_ue(b);
    if (s->poc_type == 0) {
        s->log2_max_poc_lsb = (int)get_ue(b) + 4;
    } else if (s->poc_type == 1) {
        uint32_t n;
        get_u(b, 1);
        get_se(b);
        get_se(b);
        n = get_ue(b);
        for (i = 0; i < n && i < 256; i++)
            get_se(b);
    }
    s->max_num_ref_frames = (int)get_ue(b);
    get_u(b, 1);
    w_mbs = get_ue(b) + 1;
    h_units = get_ue(b) + 1;
    s->frame_mbs_only = (int)get_u(b, 1);
    if (!s->frame_mbs_only)
        get_u(b, 1);
    get_u(b, 1);
    s->width = (int)w_mbs * 16;
    s->height = (int)h_units * 16 * (2 - s->frame_mbs_only);
    if (get_u(b, 1)) {
        int cx = chroma == 0 || chroma == 3 ? 1 : 2;
        int cy = (chroma == 1 ? 2 : 1) * (2 - s->frame_mbs_only);
        uint32_t l = get_ue(b), r = get_ue(b), t = get_ue(b), bo = get_ue(b);
        s->width -= (int)(l + r) * cx;
        s->height -= (int)(t + bo) * cy;
    }
    if (get_u(b, 1))
        parse_vui(b, s);
    s->valid = 1;
}

static int parse_sei_pic_struct(bits *b, const h264_sps *s)
{
    while (b->pos + 16 <= b->size * 8) {
        uint32_t type = 0, size = 0, byte, end;
        while ((byte = get_u(b, 8)) == 0xFF)
            type += 255;
        type += byte;
        while ((byte = get_u(b, 8)) == 0xFF)
            size += 255;
        size += byte;
        end = b->pos + size * 8;
        if (type == SEI_PIC_TIMING && s->valid) {
            if (s->cpb_dpb_delays_present)
                get_u(b, s->cpb_removal_delay_len + s->dpb_output_delay_len);
            return s->pic_struct_present ? (int)get_u(b, 4) : -1;
        }
        b->pos = end;
    }
    return -1;
}

void h264_parser_init(h264_parser *p)
{
    memset(p, 0, sizeof(*p));
}

typedef struct {
    h264_parser *p;
    h264_au_info *info;
    int found_slice;
} au_ctx;

static void on_nal(void *user, const uint8_t *nal, uint32_t size)
{
    au_ctx *c = (au_ctx *)user;
    h264_parser *p = c->p;
    h264_au_info *info = c->info;
    bits b;
    int type;

    if (size < 1)
        return;
    type = nal[0] & 0x1F;
    if (type == NAL_SPS) {
        rbsp_load(&b, nal, size);
        parse_sps(&b, &p->sps);
        if (size <= H264_MAX_PARAM_SET) {
            memcpy(p->sps_nal, nal, size);
            p->sps_size = size;
        }
        info->has_sps = 1;
    } else if (type == NAL_PPS) {
        if (size <= H264_MAX_PARAM_SET) {
            memcpy(p->pps_nal, nal, size);
            p->pps_size = size;
        }
    } else if (type == NAL_SEI) {
        int ps;
        rbsp_load(&b, nal, size);
        if ((ps = parse_sei_pic_struct(&b, &p->sps)) >= 0)
            info->pic_struct = ps;
    } else if ((type == NAL_SLICE || type == NAL_IDR) && !c->found_slice && p->sps.valid) {
        const h264_sps *s = &p->sps;
        rbsp_load(&b, nal, size);
        c->found_slice = 1;
        info->is_idr = type == NAL_IDR;
        info->nal_ref_idc = (nal[0] >> 5) & 3;
        get_ue(&b);
        info->slice_type = (int)(get_ue(&b) % 5);
        if (info->slice_type > H264_SLICE_I)
            info->slice_type -= 3;   /* SP and SI map to P and I */
        get_ue(&b);
        info->frame_num = (int)get_u(&b, s->log2_max_frame_num);
        if (!s->frame_mbs_only) {
            info->field_pic = (int)get_u(&b, 1);
            if (info->field_pic)
                info->bottom_field = (int)get_u(&b, 1);
        }
        if (info->is_idr)
            get_ue(&b);
        if (s->poc_type == 0) {
            int max_lsb = 1 << s->log2_max_poc_lsb, msb;
            info->poc_lsb = (int)get_u(&b, s->log2_max_poc_lsb);
            if (info->is_idr) {
                p->prev_poc_msb = 0;
                p->prev_poc_lsb = 0;
            }
            if (info->poc_lsb < p->prev_poc_lsb && p->prev_poc_lsb - info->poc_lsb >= max_lsb / 2)
                msb = p->prev_poc_msb + max_lsb;
            else if (info->poc_lsb > p->prev_poc_lsb && info->poc_lsb - p->prev_poc_lsb > max_lsb / 2)
                msb = p->prev_poc_msb - max_lsb;
            else
                msb = p->prev_poc_msb;
            info->poc = msb + info->poc_lsb;
            if (info->nal_ref_idc) {
                p->prev_poc_msb = msb;
                p->prev_poc_lsb = info->poc_lsb;
            }
        } else if (s->poc_type == 2) {
            /* Output order is decode order: the POC follows frame_num (ICON1.PMF uses this). */
            if (info->is_idr)
                p->frame_num_offset = 0;
            else if (p->prev_frame_num > info->frame_num)
                p->frame_num_offset += 1 << s->log2_max_frame_num;
            info->poc = info->is_idr ? 0 : 2 * (p->frame_num_offset + info->frame_num) - (info->nal_ref_idc == 0);
            p->prev_frame_num = info->frame_num;
        }
    }
}

void h264_each_nal(const uint8_t *buf, uint32_t size, h264_nal_fn fn, void *user)
{
    uint32_t i = 0, start = 0;
    int in_nal = 0;

    while (i + 3 <= size) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            if (in_nal) {
                uint32_t end = i;
                while (end > start && buf[end - 1] == 0)
                    end--;
                fn(user, buf + start, end - start);
            }
            i += 3;
            start = i;
            in_nal = 1;
        } else {
            i++;
        }
    }
    if (in_nal && start < size)
        fn(user, buf + start, size - start);
}

int h264_parse_au(h264_parser *p, const uint8_t *au, uint32_t size, h264_au_info *out)
{
    au_ctx c;

    memset(out, 0, sizeof(*out));
    out->pic_struct = -1;
    c.p = p;
    c.info = out;
    c.found_slice = 0;
    h264_each_nal(au, size, on_nal, &c);
    return c.found_slice ? 0 : -1;
}
