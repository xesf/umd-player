#include "vtiming.h"

#include <string.h>

#include "mpeg_ps.h"

static uint8_t field_count(const h264_au_info *info)
{
    static const uint8_t by_pic_struct[9] = {2, 1, 1, 2, 2, 3, 3, 4, 6};

    if (info->pic_struct >= 0 && info->pic_struct <= 8)
        return by_pic_struct[info->pic_struct];
    return info->field_pic ? 1 : 2;
}

void vtiming_reset(vtiming *v, const h264_sps *sps)
{
    memset(v, 0, sizeof(*v));
    v->field_ticks = 3003;
    if (sps && sps->num_units_in_tick && sps->time_scale)
        v->field_ticks = (uint32_t)((180000ull * sps->num_units_in_tick + sps->time_scale / 2) / sps->time_scale);
}

static vtiming_entry *find(vtiming *v, uint32_t id)
{
    uint32_t first;

    if (v->count == 0)
        return NULL;
    first = v->e[0].id;
    if (id < first || id - first >= v->count)
        return NULL;
    return &v->e[id - first];
}

/* The display-order predecessor: the highest POC below ours in the same epoch, or the highest
   POC of the previous epoch. NULL if it is not in the window. */
static const vtiming_entry *predecessor(const vtiming *v, const vtiming_entry *x)
{
    const vtiming_entry *best = NULL, *prev_epoch = NULL;

    for (uint32_t i = 0; i < v->count; i++) {
        const vtiming_entry *c = &v->e[i];
        if (c == x)
            continue;
        if (c->epoch == x->epoch && c->poc < x->poc && (!best || c->poc > best->poc))
            best = c;
        if (c->epoch + 1 == x->epoch && (!prev_epoch || c->poc > prev_epoch->poc))
            prev_epoch = c;
    }
    return best ? best : prev_epoch;
}

static int settled(const vtiming *v, const vtiming_entry *x)
{
    return v->next_id - x->id > VTIMING_SETTLE;
}

static int64_t abs64(int64_t a)
{
    return a < 0 ? -a : a;
}

static void decide(vtiming *v, vtiming_entry *x)
{
    int64_t diff;

    if (!x->anchored) {
        x->t = x->predicted;
        return;
    }
    if (x->predicted == VTIMING_UNKNOWN) {
        x->t = x->anchor;
        return;
    }
    diff = x->anchor - x->predicted;
    if (abs64(diff) <= 1) {
        x->t = x->anchor;
        v->pending_jump = 0;
    } else if (v->pending_jump && abs64(diff - v->pending_jump) <= 1) {
        x->t = x->anchor;
        v->pending_jump = 0;
        v->jumps++;
    } else {
        x->t = x->predicted;
        v->pending_jump = diff;
        v->outliers++;
    }
}

static void resolve(vtiming *v)
{
    int changed = 1;

    while (changed) {
        changed = 0;
        for (uint32_t i = 0; i < v->count; i++) {
            vtiming_entry *x = &v->e[i];
            const vtiming_entry *p;

            if (x->t != VTIMING_UNKNOWN || x->lost || !settled(v, x))
                continue;
            p = predecessor(v, x);
            if (p && p->lost)
                p = NULL;            /* a lost predecessor: only our own PTS can time us */
            if (p && p->t == VTIMING_UNKNOWN)
                continue;
            if (p) {
                x->predicted = p->t + (int64_t)p->fields * v->field_ticks;
            } else if (!x->anchored) {
                /* Nothing before it to count from and no PTS: a leading picture after a seek or at
                   the start (an open GOP's B pictures shown before their I picture). It can never
                   be timed. Marking it lost lets the pictures after it fall back on their PTS. */
                x->lost = 1;
                changed = 1;
                continue;
            }
            decide(v, x);
            changed = 1;
        }
    }
}

uint32_t vtiming_push(vtiming *v, const h264_au_info *info, uint64_t pes_pts)
{
    vtiming_entry *x;

    if (info->is_idr && v->next_id > 0)
        v->epoch++;
    if (v->count == VTIMING_WINDOW) {
        memmove(&v->e[0], &v->e[1], (VTIMING_WINDOW - 1) * sizeof(v->e[0]));
        v->count--;
    }
    x = &v->e[v->count++];
    x->id = v->next_id++;
    x->epoch = v->epoch;
    x->poc = info->poc;
    x->fields = field_count(info);
    x->anchored = pes_pts != PS_NO_TS;
    x->anchor = x->anchored ? (int64_t)pes_pts * 2 : VTIMING_UNKNOWN;
    x->t = VTIMING_UNKNOWN;
    x->predicted = VTIMING_UNKNOWN;
    x->lost = 0;
    resolve(v);
    return x->id;
}

int vtiming_get(const vtiming *v, uint32_t id, uint64_t *pts90)
{
    const vtiming_entry *x = find((vtiming *)v, id);

    if (!x)
        return -1;
    if (x->t != VTIMING_UNKNOWN) {
        *pts90 = (uint64_t)(x->t / 2);
        return 1;
    }
    if (x->lost)
        return -1;
    /* A chain through not-yet-settled pictures can take a few more pushes; after 3 windows it is lost. */
    return v->next_id - x->id > 3 * VTIMING_SETTLE ? -1 : 0;
}

int64_t vtiming_predicted(const vtiming *v, uint32_t id)
{
    const vtiming_entry *x = find((vtiming *)v, id);
    return x && x->predicted != VTIMING_UNKNOWN ? x->predicted / 2 : -1;
}

int64_t vtiming_anchor(const vtiming *v, uint32_t id)
{
    const vtiming_entry *x = find((vtiming *)v, id);
    return x && x->anchored ? x->anchor / 2 : -1;
}
