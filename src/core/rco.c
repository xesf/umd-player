#include "rco.h"

#include <stdlib.h>
#include <string.h>

#define HEADER_SIZE 0x28
#define REF_EVENT   0x400
#define REF_IMAGE   0x402
#define REF_OBJECT  0x407
#define REF_ANIM    0x408
#define REF_ANIMOBJ 0x409

enum { TYPE_MAIN = 1, TYPE_SCRIPT = 2, TYPE_IMAGE = 4, TYPE_OBJECT = 8, TYPE_ANIM = 9 };

typedef struct parser {
    rco *r;
    const char *labels;
    uint32_t labels_size;
    const char *events;
    uint32_t events_size;
    uint32_t img_data, img_size;
    uint32_t *offsets;        /* entry offset of each widget, to resolve references */
    uint32_t *anim_offsets;   /* and of each animation */
    int anim_offset_cap;
    int object_cap, image_cap, anim_cap, step_cap;
    int failed;
} parser;

static uint32_t rd32(const rco *r, uint32_t off)
{
    if (off + 4 > r->file_size)
        return 0;
    return (uint32_t)r->file[off] | (uint32_t)r->file[off + 1] << 8 | (uint32_t)r->file[off + 2] << 16 |
           (uint32_t)r->file[off + 3] << 24;
}

static float rdf(const rco *r, uint32_t off)
{
    uint32_t v = rd32(r, off);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static const char *string_at(const char *base, uint32_t size, uint32_t off)
{
    if (!base || off >= size || !memchr(base + off, 0, size - off))
        return NULL;
    return base + off;
}

/* Makes room for one more item. Returns 0, or -1 when out of memory. */
static int grow(void **p, int count, int *cap, size_t item)
{
    void *n;

    if (count < *cap)
        return 0;
    if ((n = realloc(*p, (size_t)(*cap ? *cap * 2 : 32) * item)) == NULL)
        return -1;
    *p = n;
    *cap = *cap ? *cap * 2 : 32;
    return 0;
}

/* Widget references hold the entry offset; turn them into indexes once all widgets are read. */
static int object_at(const parser *ps, uint32_t off)
{
    for (int i = 0; i < ps->r->object_count; i++)
        if (ps->offsets[i] == off)
            return i;
    return -1;
}

static rco_action read_action(parser *ps, uint32_t off, int *pending, int *pending_anim)
{
    rco_action a = {NULL, -1, -1};
    uint32_t type = rd32(ps->r, off), ptr = rd32(ps->r, off + 4);

    if (type == REF_EVENT)
        a.event = string_at(ps->events, ps->events_size, ptr);
    else if ((type == REF_OBJECT || type == REF_ANIMOBJ) && ptr != 0xFFFFFFFF)
        *pending = (int)ptr;  /* resolved later */
    else if (type == REF_ANIM && ptr != 0xFFFFFFFF)
        *pending_anim = (int)ptr;
    return a;
}

static int anim_at(const parser *ps, uint32_t off)
{
    for (int i = 0; i < ps->r->anim_count; i++)
        if (ps->anim_offsets[i] == off)
            return i;
    return -1;
}

static void read_object(parser *ps, uint32_t off, int sub, const char *name, int parent)
{
    rco *r = ps->r;
    rco_object *o;
    uint32_t d = off + HEADER_SIZE;
    int pending[RCO_BTN_ACTIONS], pending_anim[RCO_BTN_ACTIONS], cap = ps->object_cap;

    if (grow((void **)&r->objects, r->object_count, &ps->object_cap, sizeof(rco_object)) < 0 ||
        grow((void **)&ps->offsets, r->object_count, &cap, sizeof(uint32_t)) < 0) {
        ps->failed = 1;
        return;
    }
    o = &r->objects[r->object_count];
    memset(o, 0, sizeof(*o));
    o->kind = sub;
    o->name = name;
    o->parent = parent;
    o->first_child = o->next_sibling = -1;
    o->image = -1;
    for (int i = 0; i < RCO_BTN_ACTIONS; i++) {
        o->action[i].object = -1;
        o->action[i].anim = -1;
        pending[i] = pending_anim[i] = -1;
    }
    o->scale[0] = o->scale[1] = o->scale[2] = 1;
    o->color[0] = o->color[1] = o->color[2] = o->color[3] = 1;
    if (sub == RCO_PAGE) {
        for (int i = 0; i < RCO_PAGE_ACTIONS; i++)
            o->action[i] = read_action(ps, d + 4 + (uint32_t)i * 8, &pending[i], &pending_anim[i]);
    } else {
        /* Shared by groups, planes and buttons: position, colour, size, scale. */
        for (int i = 0; i < 3; i++)
            o->pos[i] = rdf(r, d + (uint32_t)i * 4);
        for (int i = 0; i < 4; i++)
            o->color[i] = rdf(r, d + 12 + (uint32_t)i * 4);
        for (int i = 0; i < 3; i++)
            o->size[i] = rdf(r, d + 28 + (uint32_t)i * 4);
        for (int i = 0; i < 3; i++)
            o->scale[i] = rdf(r, d + 40 + (uint32_t)i * 4);
        o->anchor = (int)rd32(r, d + 52);
        if (sub == RCO_PLANE || sub == RCO_BUTTON) {
            uint32_t type = rd32(r, d + 64), ptr = rd32(r, d + 68);
            if (type == REF_IMAGE && ptr != 0xFFFFFFFF)
                o->image = (int)ptr;     /* an entry offset until rco_parse maps it */
        }
        if (sub == RCO_BUTTON)
            for (int i = 0; i < RCO_BTN_ACTIONS; i++)
                o->action[i] = read_action(ps, d + 72 + (uint32_t)i * 8, &pending[i], &pending_anim[i]);
    }
    for (int i = 0; i < RCO_BTN_ACTIONS; i++) {
        o->action[i].object = pending[i];          /* still entry offsets until rco_parse maps them */
        o->action[i].anim = pending_anim[i];
    }
    ps->offsets[r->object_count] = off;
    r->object_count++;
}

static void read_step(parser *ps, uint32_t off, int sub)
{
    rco *r = ps->r;
    rco_step *s;
    uint32_t d = off + HEADER_SIZE;

    if (grow((void **)&r->steps, r->step_count, &ps->step_cap, sizeof(rco_step)) < 0) {
        ps->failed = 1;
        return;
    }
    s = &r->steps[r->step_count++];
    memset(s, 0, sizeof(*s));
    s->kind = sub;
    s->object = -1;
    s->anim = -1;
    switch (sub) {
    case RCO_STEP_MOVE:
    case RCO_STEP_COLOR:
    case RCO_STEP_SCALE:
    case RCO_STEP_FADE: {
        uint32_t type = rd32(r, d), ptr = rd32(r, d + 4);
        int n = sub == RCO_STEP_COLOR ? 4 : sub == RCO_STEP_FADE ? 1 : 3;
        if ((type == REF_ANIMOBJ || type == REF_OBJECT) && ptr != 0xFFFFFFFF)
            s->object = (int)ptr;
        s->duration = rdf(r, d + 8);
        s->accel = rdf(r, d + 12);
        for (int i = 0; i < n; i++)
            s->value[i] = rdf(r, d + 16 + (uint32_t)i * 4);
        break;
    }
    case RCO_STEP_DELAY:
        s->duration = rdf(r, d);
        break;
    case RCO_STEP_EVENT:
        if (rd32(r, d) == REF_EVENT)
            s->event = string_at(ps->events, ps->events_size, rd32(r, d + 4));
        else if (rd32(r, d) == REF_ANIM && rd32(r, d + 4) != 0xFFFFFFFF)
            s->anim = (int)rd32(r, d + 4);   /* an entry offset until rco_parse maps it */
        break;
    default:
        break;
    }
}

/* Walks one entry and its children. Returns the offset to its next sibling, 0 for the last. */
static uint32_t walk(parser *ps, uint32_t off, int parent_object, int anim)
{
    rco *r = ps->r;
    uint32_t sub, type, label, head, size, children, next, child;
    const char *name;
    int me = parent_object;

    if (ps->failed || off + HEADER_SIZE > r->file_size)
        return 0;
    sub = r->file[off];
    type = r->file[off + 1];
    label = rd32(r, off + 4);
    head = rd32(r, off + 8);
    size = rd32(r, off + 12);
    children = rd32(r, off + 16);
    next = rd32(r, off + 20);
    name = label == 0xFFFFFFFF ? NULL : string_at(ps->labels, ps->labels_size, label);

    if (sub && type == TYPE_SCRIPT && !r->script) {
        uint32_t start = rd32(r, off + HEADER_SIZE), len = rd32(r, off + HEADER_SIZE + 4);
        if (off + HEADER_SIZE + 8 + start + len <= r->file_size) {
            r->script = r->file + off + HEADER_SIZE + 8 + start;
            r->script_size = len;
        }
    } else if (sub && type == TYPE_IMAGE) {
        /* Format and compression (16 bits each), size, and offset into the image data. */
        uint32_t isize = rd32(r, off + HEADER_SIZE + 4), istart = rd32(r, off + HEADER_SIZE + 8);
        rco_image *im;
        if (grow((void **)&r->images, r->image_count, &ps->image_cap, sizeof(rco_image)) < 0) {
            ps->failed = 1;
            return 0;
        }
        im = &r->images[r->image_count++];
        im->name = name;
        im->data = NULL;
        im->size = 0;
        if (istart + isize <= ps->img_size && ps->img_data + istart + isize <= r->file_size) {
            im->data = r->file + ps->img_data + istart;
            im->size = isize;
        }
    } else if (sub && type == TYPE_OBJECT) {
        read_object(ps, off, (int)sub, name, parent_object);
        me = r->object_count - 1;
        if (parent_object >= 0) {
            int *link = &r->objects[parent_object].first_child;
            while (*link >= 0)
                link = &r->objects[*link].next_sibling;
            *link = me;
        }
    } else if (type == TYPE_ANIM && sub == 1) {
        int cap = ps->anim_cap;
        if (grow((void **)&ps->anim_offsets, r->anim_count, &ps->anim_offset_cap, sizeof(uint32_t)) < 0 ||
            grow((void **)&r->anims, r->anim_count, &cap, sizeof(rco_anim)) < 0) {
            ps->failed = 1;
            return 0;
        }
        r->anims[r->anim_count].name = name;
        r->anims[r->anim_count].first_step = r->step_count;
        r->anims[r->anim_count].step_count = 0;
        ps->anim_offsets[r->anim_count] = off;
        ps->anim_cap = cap;
        anim = r->anim_count++;
    } else if (type == TYPE_ANIM && sub > 1 && anim >= 0) {
        read_step(ps, off, (int)sub);
        r->anims[anim].step_count++;
    }

    child = off + (size ? size : head);
    for (uint32_t i = 0; i < children && !ps->failed; i++) {
        uint32_t n = walk(ps, child, type == TYPE_OBJECT && sub ? me : parent_object, anim);
        if (!n)
            break;
        child += n;
    }
    return next;
}

/* Image references hold the image entry's offset. Images come in file order, so walk the
   image table again to map offsets to indexes. */
static int image_at(const rco *r, uint32_t img_table, uint32_t off)
{
    uint32_t child, count, n;

    if (img_table == 0xFFFFFFFF)
        return -1;
    count = rd32(r, img_table + 16);
    child = img_table + (rd32(r, img_table + 12) ? rd32(r, img_table + 12) : rd32(r, img_table + 8));
    for (uint32_t i = 0; i < count; i++) {
        if (child == off)
            return (int)i;
        if ((n = rd32(r, child + 20)) == 0)
            break;
        child += n;
    }
    return -1;
}

int rco_parse(rco *r, const uint8_t *data, uint32_t size)
{
    parser ps;
    uint32_t img_table;

    memset(r, 0, sizeof(*r));
    if (size < 0xA4 || memcmp(data, "\0PRF", 4) != 0)
        return -1;
    r->file = data;
    r->file_size = size;
    memset(&ps, 0, sizeof(ps));
    ps.r = r;
    if (rd32(r, 0x40) + rd32(r, 0x44) <= size) {
        ps.labels = (const char *)data + rd32(r, 0x40);
        ps.labels_size = rd32(r, 0x44);
    }
    if (rd32(r, 0x48) + rd32(r, 0x4C) <= size) {
        ps.events = (const char *)data + rd32(r, 0x48);
        ps.events_size = rd32(r, 0x4C);
    }
    ps.img_data = rd32(r, 0x80);
    ps.img_size = rd32(r, 0x84);
    img_table = rd32(r, 0x24);
    walk(&ps, rd32(r, 0x10), -1, -1);
    if (ps.failed) {
        free(ps.offsets);
        free(ps.anim_offsets);
        rco_free(r);
        return -1;
    }
    for (int i = 0; i < r->object_count; i++) {
        rco_object *o = &r->objects[i];
        if (o->image >= 0)
            o->image = image_at(r, img_table, (uint32_t)o->image);
        for (int a = 0; a < RCO_BTN_ACTIONS; a++) {
            if (o->action[a].object >= 0)
                o->action[a].object = object_at(&ps, (uint32_t)o->action[a].object);
            if (o->action[a].anim >= 0)
                o->action[a].anim = anim_at(&ps, (uint32_t)o->action[a].anim);
        }
    }
    for (int i = 0; i < r->step_count; i++)
        if (r->steps[i].anim >= 0)
            r->steps[i].anim = anim_at(&ps, (uint32_t)r->steps[i].anim);
    for (int i = 0; i < r->step_count; i++)
        if (r->steps[i].object >= 0)
            r->steps[i].object = object_at(&ps, (uint32_t)r->steps[i].object);
    free(ps.offsets);
    free(ps.anim_offsets);
    return 0;
}

void rco_free(rco *r)
{
    free(r->images);
    free(r->objects);
    free(r->anims);
    free(r->steps);
    memset(r, 0, sizeof(*r));
}

int rco_find_object(const rco *r, const char *name)
{
    for (int i = 0; i < r->object_count; i++)
        if (r->objects[i].name && strcmp(r->objects[i].name, name) == 0)
            return i;
    return -1;
}

int rco_find_anim(const rco *r, const char *name)
{
    for (int i = 0; i < r->anim_count; i++)
        if (r->anims[i].name && strcmp(r->anims[i].name, name) == 0)
            return i;
    return -1;
}
