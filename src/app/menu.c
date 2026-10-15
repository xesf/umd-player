#include "menu.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/jpeg.h"
#include "../core/png.h"
#include "../core/rco.h"
#include "../core/vsmx.h"
#include "../platform/platform.h"

#define MAX_TWEENS   256
#define MAX_RUNS     32
#define MAX_TIMERS   32
#define MAX_PAGES    16
#define MAX_USERDATA 64
#define UNITS_W      854.0f           /* menu coordinates: a 16:9 screen 480 units high */
#define UNITS_H      480.0f
#define START_RCO    "EN100000"

enum { P_POS, P_COLOR, P_SCALE, P_ROT, P_CROP, P_SIZE, P_ALPHA };

/* What the script can change on a widget. */
typedef struct wstate {
    float pos[3], color[4], size[3], scale[3], rot[3], crop[4];
    int image, has_crop;
    int hidden;               /* widget.visible = false: it and its children are not drawn */
} wstate;

typedef struct tween {
    int active, widget, prop, n;
    float from[4], to[4], dur;
    uint64_t start;
} tween;

typedef struct anim_run {
    int active, anim, step;
    int target;               /* anim.play(widget): the widget for steps that name none */
    uint64_t wait_until;
} anim_run;

typedef struct mtimer {
    int used, id, fn, self;   /* fn and self: vs_hold handles */
    uint32_t interval;
    uint64_t due;
} mtimer;

typedef struct ref {
    menu *m;
    int index;                /* widget, animation or image index */
} ref;

typedef struct user_value {
    int type;                 /* VS_NUM, VS_STR, VS_BOOL, or VS_UNDEF */
    double n;
    char s[64];
} user_value;

/* One RCO package with its script. */
typedef struct res {
    char name[16];
    uint8_t *file;
    rco r;
    vsmx *vm;
    wstate *w;
    ref *wref, *aref, *iref;
    uint8_t **rgba;           /* decoded images, on first use */
    int *img_w, *img_h;
    int pages[MAX_PAGES], page_count;   /* open pages, in drawing order */
    int active;               /* page that takes input, or -1 */
    int *focus;               /* per page: the focused button, or -1 */
} res;

struct menu {
    menu_player p;
    char language[3];
    res *res;
    char pending[16];         /* a resource change asked for during a script call */
    int ended;
    tween tweens[MAX_TWEENS];
    anim_run runs[MAX_RUNS];
    mtimer timers[MAX_TIMERS];
    int next_timer;
    int lock;                 /* input locks from animations */
    uint64_t now;
    int dirty;
    user_value user[MAX_USERDATA];
    int user_count;
    int resume_playlist, resume_chapter;   /* for getResumeInfo */
    int audio_id, sub_id, sub_on;
    float win[4];
    int win_set;
    int drawn[4];             /* x0 y0 x1 y1 of what the last menu_draw put on the canvas */
};

static void mlog(menu *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void mlog(menu *m, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (m->p.log)
        m->p.log(m->p.user, buf);
}

static void vm_log(void *user, const char *msg)
{
    mlog((menu *)user, "menu: %s", msg);
}

static vs_value arg(const vs_value *args, int argc, int i)
{
    return i < argc ? args[i] : vs_undef();
}

static float argf(const vs_value *args, int argc, int i, float def)
{
    return i < argc && args[i].type != VS_UNDEF ? (float)vs_number(args[i]) : def;
}

/* ---- widgets ---- */

static void reset_widget(res *r, int i)
{
    const rco_object *o = &r->r.objects[i];
    wstate *w = &r->w[i];

    memcpy(w->pos, o->pos, sizeof(w->pos));
    memcpy(w->color, o->color, sizeof(w->color));
    memcpy(w->size, o->size, sizeof(w->size));
    memcpy(w->scale, o->scale, sizeof(w->scale));
    memset(w->rot, 0, sizeof(w->rot));
    w->image = o->image;
    w->has_crop = 0;
    w->hidden = 0;
}

static void reset_tree(res *r, int i)
{
    reset_widget(r, i);
    for (int c = r->r.objects[i].first_child; c >= 0; c = r->r.objects[c].next_sibling)
        reset_tree(r, c);
}

static int page_of(const res *r, int i)
{
    while (i >= 0 && r->r.objects[i].kind != RCO_PAGE)
        i = r->r.objects[i].parent;
    return i;
}

static float *prop_ptr(wstate *w, int prop, int *n)
{
    switch (prop) {
    case P_POS: *n = 3; return w->pos;
    case P_COLOR: *n = 4; return w->color;
    case P_SCALE: *n = 3; return w->scale;
    case P_ROT: *n = 3; return w->rot;
    case P_CROP: *n = 4; return w->crop;
    case P_SIZE: *n = 3; return w->size;
    default: *n = 1; return &w->color[3];
    }
}

static void start_tween(menu *m, int widget, int prop, const float *to, int n, float dur)
{
    res *r = m->res;
    tween *t = NULL;
    float *cur;
    int count;

    if (widget < 0 || widget >= r->r.object_count)
        return;
    if (prop == P_CROP)
        r->w[widget].has_crop = 1;
    cur = prop_ptr(&r->w[widget], prop, &count);
    if (n > count)
        n = count;
    if (dur <= 0) {
        memcpy(cur, to, (size_t)n * sizeof(float));
        m->dirty = 1;
        return;
    }
    for (int i = 0; i < MAX_TWEENS; i++)     /* a new tween replaces one on the same property */
        if (m->tweens[i].active && m->tweens[i].widget == widget && m->tweens[i].prop == prop) {
            t = &m->tweens[i];
            break;
        }
    for (int i = 0; !t && i < MAX_TWEENS; i++)
        if (!m->tweens[i].active)
            t = &m->tweens[i];
    if (!t) {
        memcpy(cur, to, (size_t)n * sizeof(float));
        return;
    }
    t->active = 1;
    t->widget = widget;
    t->prop = prop;
    t->n = n;
    memcpy(t->from, cur, (size_t)n * sizeof(float));
    memcpy(t->to, to, (size_t)n * sizeof(float));
    t->dur = dur;
    t->start = m->now;
}

static void step_tweens(menu *m)
{
    for (int i = 0; i < MAX_TWEENS; i++) {
        tween *t = &m->tweens[i];
        float k, *cur;
        int n;
        if (!t->active)
            continue;
        k = (float)(m->now - t->start) / 1000.0f / t->dur;
        if (k >= 1) {
            k = 1;
            t->active = 0;
        }
        cur = prop_ptr(&m->res->w[t->widget], t->prop, &n);
        for (int c = 0; c < t->n; c++)
            cur[c] = t->from[c] + (t->to[c] - t->from[c]) * k;
        m->dirty = 1;
    }
}

/* ---- script calls ---- */

static void apply_pending(menu *m);

/* Calls an event handler named in the RCO ("script:/main/onPush_btnRootMenu"). */
static void call_event(menu *m, const char *event, int widget)
{
    const char *name;
    vs_value w = vs_undef();
    res *r = m->res;

    if (!event)
        return;
    name = strrchr(event, '/');
    name = name ? name + 1 : event;
    if (widget >= 0)
        w = vs_get_index(r->vm, vs_global(r->vm, "\001widgets"), widget);
    if (vs_call(r->vm, vs_global(r->vm, name), w, &w, 1, NULL) < 0)
        mlog(m, "menu: no handler %s", name);
    apply_pending(m);
}

static void call_value(menu *m, vs_value fn, vs_value self, const vs_value *args, int argc)
{
    vs_call(m->res->vm, fn, self, args, argc, NULL);
    apply_pending(m);
}

/* ---- animations ---- */

static void play_anim(menu *m, int anim, int target)
{
    anim_run *run = NULL;

    for (int i = 0; i < MAX_RUNS; i++)
        if (m->runs[i].active && m->runs[i].anim == anim)
            run = &m->runs[i];
    for (int i = 0; !run && i < MAX_RUNS; i++)
        if (!m->runs[i].active)
            run = &m->runs[i];
    if (!run)
        return;
    run->active = 1;
    run->anim = anim;
    run->target = target;
    run->step = 0;
    run->wait_until = m->now;
}

static void step_anims(menu *m)
{
    for (int i = 0; i < MAX_RUNS; i++) {
        anim_run *run = &m->runs[i];
        res *r = m->res;
        while (run->active && m->now >= run->wait_until) {
            const rco_anim *a = &r->r.anims[run->anim];
            const rco_step *s;
            int obj;
            if (run->step >= a->step_count) {
                run->active = 0;
                break;
            }
            s = &r->r.steps[a->first_step + run->step++];
            obj = s->object >= 0 ? s->object : run->target;
            switch (s->kind) {
            case RCO_STEP_MOVE: start_tween(m, obj, P_POS, s->value, 3, s->duration); break;
            case RCO_STEP_COLOR: start_tween(m, obj, P_COLOR, s->value, 4, s->duration); break;
            case RCO_STEP_SCALE: start_tween(m, obj, P_SCALE, s->value, 3, s->duration); break;
            case RCO_STEP_FADE: start_tween(m, obj, P_ALPHA, s->value, 1, s->duration); break;
            case RCO_STEP_DELAY: run->wait_until = m->now + (uint64_t)(s->duration * 1000); break;
            case RCO_STEP_LOCK: m->lock++; break;
            case RCO_STEP_UNLOCK: if (m->lock > 0) m->lock--; break;
            case RCO_STEP_EVENT:
                call_event(m, s->event, -1);
                if (m->res != r)
                    return;   /* the event changed the resource */
                break;
            default: break;
            }
        }
    }
}

/* ---- focus and input ---- */

static void set_focus(menu *m, int widget)
{
    res *r = m->res;
    int page = page_of(r, widget), old;

    if (page < 0)
        return;
    old = r->focus[page];
    if (old == widget)
        return;
    r->focus[page] = widget;
    m->dirty = 1;
    if (old >= 0)
        call_event(m, r->r.objects[old].action[RCO_BTN_FOCUS_OUT].event, old);
    if (m->res == r)
        call_event(m, r->r.objects[widget].action[RCO_BTN_FOCUS_IN].event, widget);
}

static int first_button(const res *r, int i)
{
    if (r->r.objects[i].kind == RCO_BUTTON)
        return i;
    for (int c = r->r.objects[i].first_child; c >= 0; c = r->r.objects[c].next_sibling) {
        int b = first_button(r, c);
        if (b >= 0)
            return b;
    }
    return -1;
}

static void do_input(menu *m, uint32_t pressed)
{
    static const struct { uint32_t button; int action; } dirs[] = {
        {PLAT_BTN_LEFT, RCO_BTN_LEFT}, {PLAT_BTN_RIGHT, RCO_BTN_RIGHT}, {PLAT_BTN_UP, RCO_BTN_UP},
        {PLAT_BTN_DOWN, RCO_BTN_DOWN}};
    res *r = m->res;
    int page = r->active, focus;

    if (page < 0 || m->lock || !pressed)
        return;
    focus = r->focus[page];
    if (focus < 0 && (focus = first_button(r, page)) >= 0)
        set_focus(m, focus);
    if (m->res != r)
        return;
    if (pressed & PLAT_BTN_CIRCLE) {
        call_event(m, r->r.objects[page].action[RCO_PAGE_CANCEL].event, page);
        return;
    }
    if (focus < 0)
        return;
    if (pressed & PLAT_BTN_CROSS) {
        call_event(m, r->r.objects[focus].action[RCO_BTN_PUSH].event, focus);
        return;
    }
    for (unsigned i = 0; i < 4; i++)
        if (pressed & dirs[i].button) {
            const rco_action *a = &r->r.objects[focus].action[dirs[i].action];
            if (a->object >= 0)
                set_focus(m, a->object);
            else if (a->event)
                call_event(m, a->event, focus);
            return;
        }
}

/* ---- host objects ---- */

static const vs_class children_class, anim_class, table_class, texture_class;
static const vs_class widget_class;

static vs_value widget_value(menu *m, int i)
{
    if (i < 0 || i >= m->res->r.object_count)
        return vs_undef();
    return vs_get_index(m->res->vm, vs_global(m->res->vm, "\001widgets"), i);
}

static vs_value array3(vsmx *vm, const float *v, int n)
{
    vs_value a = vs_new_array(vm, n);
    for (int i = 0; i < n; i++)
        vs_set_index(vm, a, i, vs_num(v[i]));
    return a;
}

#define NATIVE(name) static void name(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret)

NATIVE(w_open)
{
    ref *rf = user;
    res *r = rf->m->res;
    (void)vm; (void)args; (void)argc;
    *ret = self;              /* page.open().activate() */
    for (int i = 0; i < r->page_count; i++)
        if (r->pages[i] == rf->index)
            return;
    mlog(rf->m, "menu: open page %s", r->r.objects[rf->index].name ? r->r.objects[rf->index].name : "?");
    if (r->page_count < MAX_PAGES) {
        reset_tree(r, rf->index);
        r->focus[rf->index] = -1;
        r->pages[r->page_count++] = rf->index;
        call_event(rf->m, r->r.objects[rf->index].action[RCO_PAGE_INIT].event, rf->index);
    }
    rf->m->dirty = 1;
}

NATIVE(w_close)
{
    ref *rf = user;
    res *r = rf->m->res;
    (void)vm; (void)args; (void)argc;
    *ret = self;
    for (int i = 0; i < r->page_count; i++)
        if (r->pages[i] == rf->index) {
            memmove(&r->pages[i], &r->pages[i + 1], (size_t)(r->page_count - i - 1) * sizeof(int));
            r->page_count--;
            break;
        }
    if (r->active == rf->index)
        r->active = r->page_count ? r->pages[r->page_count - 1] : -1;
    rf->m->dirty = 1;
}

NATIVE(w_activate)
{
    ref *rf = user;
    res *r = rf->m->res;
    (void)vm; (void)args; (void)argc;
    *ret = self;
    r->active = rf->index;
    call_event(rf->m, r->r.objects[rf->index].action[RCO_PAGE_ACTIVATE].event, rf->index);
}

NATIVE(w_set_focus)
{
    ref *rf = user;
    (void)vm; (void)args; (void)argc;
    *ret = self;
    set_focus(rf->m, rf->index);
}

/* setPos(x, y, z) sets at once; animPos(x, y, z, ms) moves over time: the duration is the
   last argument, and missing values keep their current value. */
static void set_values(ref *rf, int prop, const vs_value *args, int argc, int timed, int n)
{
    float v[4], dur = 0;
    float *cur;
    int count;

    if (timed && argc > 0) {
        dur = (float)vs_number(args[argc - 1]);
        argc--;
    }
    cur = prop_ptr(&rf->m->res->w[rf->index], prop, &count);
    if (n > count)
        n = count;
    for (int i = 0; i < n; i++)
        v[i] = argf(args, argc, i, cur[i]);
    start_tween(rf->m, rf->index, prop, v, n, dur);
}

NATIVE(w_set_pos) { (void)vm; (void)self; (void)ret; set_values(user, P_POS, args, argc, 0, 3); }
NATIVE(w_set_color) { (void)vm; (void)self; (void)ret; set_values(user, P_COLOR, args, argc, 0, 4); }
NATIVE(w_set_scale) { (void)vm; (void)self; (void)ret; set_values(user, P_SCALE, args, argc, 0, 3); }
NATIVE(w_set_rotate) { (void)vm; (void)self; (void)ret; set_values(user, P_ROT, args, argc, 0, 3); }
NATIVE(w_set_size) { (void)vm; (void)self; (void)ret; set_values(user, P_SIZE, args, argc, 0, 3); }
NATIVE(w_set_crop)
{
    ref *rf = user;
    (void)vm; (void)self; (void)ret;
    rf->m->res->w[rf->index].has_crop = 1;
    set_values(rf, P_CROP, args, argc, 0, 4);
}
NATIVE(w_anim_pos) { (void)vm; (void)self; (void)ret; set_values(user, P_POS, args, argc, 1, 3); }
NATIVE(w_anim_color) { (void)vm; (void)self; (void)ret; set_values(user, P_COLOR, args, argc, 1, 4); }
NATIVE(w_anim_scale) { (void)vm; (void)self; (void)ret; set_values(user, P_SCALE, args, argc, 1, 3); }
NATIVE(w_anim_rotate) { (void)vm; (void)self; (void)ret; set_values(user, P_ROT, args, argc, 1, 3); }
NATIVE(w_anim_crop) { (void)vm; (void)self; (void)ret; set_values(user, P_CROP, args, argc, 1, 4); }

static void get_values(vsmx *vm, ref *rf, int prop, vs_value *ret)
{
    int n;
    float *v = prop_ptr(&rf->m->res->w[rf->index], prop, &n);
    *ret = array3(vm, v, n);
}

NATIVE(w_get_pos) { (void)self; (void)args; (void)argc; get_values(vm, user, P_POS, ret); }
NATIVE(w_get_color) { (void)self; (void)args; (void)argc; get_values(vm, user, P_COLOR, ret); }
NATIVE(w_get_scale) { (void)self; (void)args; (void)argc; get_values(vm, user, P_SCALE, ret); }
NATIVE(w_get_rotate) { (void)self; (void)args; (void)argc; get_values(vm, user, P_ROT, ret); }
NATIVE(w_get_size)
{
    ref *rf = user;
    res *r = rf->m->res;
    wstate *w = &r->w[rf->index];
    float s[3];
    (void)self; (void)args; (void)argc;
    memcpy(s, w->size, sizeof(s));
    if (s[0] == 0 && s[1] == 0 && w->image >= 0) {   /* the image's own size: one unit per pixel */
        int iw = 0, ih = 0;
        if (png_info(r->r.images[w->image].data, r->r.images[w->image].size, &iw, &ih) == 0) {
            s[0] = (float)iw;
            s[1] = (float)ih;
        }
    }
    *ret = array3(vm, s, 3);
}

static int widget_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    static const struct { const char *name; vs_native fn; } methods[] = {
        {"open", w_open}, {"close", w_close}, {"activate", w_activate}, {"setFocus", w_set_focus},
        {"setPos", w_set_pos}, {"getPos", w_get_pos}, {"setColor", w_set_color}, {"getColor", w_get_color},
        {"setScale", w_set_scale}, {"getScale", w_get_scale}, {"setRotate", w_set_rotate},
        {"getRotate", w_get_rotate}, {"setSize", w_set_size}, {"getSize", w_get_size}, {"setCrop", w_set_crop},
        {"animPos", w_anim_pos}, {"animColor", w_anim_color}, {"animScale", w_anim_scale},
        {"animRotate", w_anim_rotate}, {"animCrop", w_anim_crop}};
    ref *rf = host;
    res *r = rf->m->res;
    const rco_object *o = &r->r.objects[rf->index];

    if (strcmp(key, "name") == 0) {
        *out = vs_string(vm, o->name ? o->name : "");
        return 1;
    }
    if (strcmp(key, "visible") == 0) {
        *out = vs_bool(!r->w[rf->index].hidden);
        return 1;
    }
    if (strcmp(key, "children") == 0) {
        *out = vs_new_host(vm, &children_class, rf);
        return 1;
    }
    if (strcmp(key, "parent") == 0) {
        *out = widget_value(rf->m, o->parent);
        return 1;
    }
    if (strcmp(key, "root") == 0) {          /* page.root.children.group01Root: the page itself */
        *out = widget_value(rf->m, rf->index);
        return 1;
    }
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
        if (strcmp(key, methods[i].name) == 0) {
            *out = vs_new_native(vm, methods[i].fn, rf);
            return 1;
        }
    /* A child by name: page.btnRootMenu */
    for (int c = o->first_child; c >= 0; c = r->r.objects[c].next_sibling)
        if (r->r.objects[c].name && strcmp(r->r.objects[c].name, key) == 0) {
            *out = widget_value(rf->m, c);
            return 1;
        }
    return 0;
}

static int widget_set(vsmx *vm, void *host, const char *key, vs_value value)
{
    ref *rf = host;
    res *r = rf->m->res;

    (void)vm;
    if (strcmp(key, "visible") == 0) {
        r->w[rf->index].hidden = !vs_truthy(value);
        rf->m->dirty = 1;
        return 1;
    }
    if (strcmp(key, "texture") == 0) {
        ref *t = vs_host(value, &texture_class);
        r->w[rf->index].image = t ? t->index : -1;
        rf->m->dirty = 1;
        return 1;
    }
    return 0;
}

static int children_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    ref *rf = host;
    res *r = rf->m->res;
    int n = 0, want = -1;

    (void)vm;
    if (key[0] >= '0' && key[0] <= '9')
        want = atoi(key);
    for (int c = r->r.objects[rf->index].first_child; c >= 0; c = r->r.objects[c].next_sibling, n++) {
        if (n == want || (want < 0 && r->r.objects[c].name && strcmp(r->r.objects[c].name, key) == 0)) {
            *out = widget_value(rf->m, c);
            return 1;
        }
    }
    if (strcmp(key, "length") == 0) {
        *out = vs_num(n);
        return 1;
    }
    *out = vs_undef();
    return 1;
}

static const vs_class widget_class;

NATIVE(a_play)
{
    ref *rf = user;
    ref *target = argc > 0 ? vs_host(args[0], &widget_class) : NULL;
    (void)vm; (void)self; (void)ret;
    play_anim(rf->m, rf->index, target ? target->index : -1);
}

NATIVE(a_stop)
{
    ref *rf = user;
    (void)vm; (void)self; (void)args; (void)argc; (void)ret;
    for (int i = 0; i < MAX_RUNS; i++)
        if (rf->m->runs[i].active && rf->m->runs[i].anim == rf->index)
            rf->m->runs[i].active = 0;
}

static int anim_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    if (strcmp(key, "play") == 0) {
        *out = vs_new_native(vm, a_play, host);
        return 1;
    }
    if (strcmp(key, "stop") == 0 || strcmp(key, "end") == 0) {
        *out = vs_new_native(vm, a_stop, host);
        return 1;
    }
    return 0;
}

/* resource.pagetable, resource.animtable, resource.texturetable: the host is a table kind. */
static int table_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    ref *rf = host;
    res *r = rf->m->res;
    int i;

    (void)vm;
    *out = vs_undef();
    if (rf->index == 0) {
        if ((i = rco_find_object(&r->r, key)) >= 0 && r->r.objects[i].kind == RCO_PAGE)
            *out = widget_value(rf->m, i);
    } else if (rf->index == 1) {
        if ((i = rco_find_anim(&r->r, key)) >= 0)
            *out = vs_new_host(vm, &anim_class, &r->aref[i]);
    } else {
        for (i = 0; i < r->r.image_count; i++)
            if (r->r.images[i].name && strcmp(r->r.images[i].name, key) == 0)
                *out = vs_new_host(vm, &texture_class, &r->iref[i]);
    }
    /* Scripts probe for optional pages (p_CMMM_2 and so on), so a miss is only worth a note. */
    if (out->type == VS_UNDEF)
        mlog(rf->m, "menu: no %s \"%s\"", rf->index == 0 ? "page" : rf->index == 1 ? "animation" : "texture", key);
    return 1;
}

static const vs_class widget_class = {"widget", widget_get, widget_set, NULL};
static const vs_class children_class = {"children", children_get, NULL, NULL};
static const vs_class anim_class = {"anim", anim_get, NULL, NULL};
static const vs_class table_class = {"table", table_get, NULL, NULL};
static const vs_class texture_class = {"texture", NULL, NULL, NULL};

/* ---- movieplayer ---- */

static int stream_number(vs_value v, int base)
{
    int n = (int)vs_number(v);
    if (v.type == VS_UNDEF || n < 0)
        return -1;
    return n >= base ? n - base : n;
}

NATIVE(mp_play)
{
    menu *m = user;
    int playlist, chapter;
    (void)vm; (void)self; (void)ret;
    /* play(0, 0, playlist, chapter[, video, audio, audio on, subtitle, subtitle on]) */
    playlist = (int)argf(args, argc, 2, 1);
    chapter = (int)argf(args, argc, 3, 1);
    if (argc >= 9) {
        if (args[5].type == VS_NUM && vs_number(args[5]) >= 0)
            m->audio_id = stream_number(args[5], 0xBD00);
        m->sub_on = vs_truthy(args[8]) && vs_number(args[7]) >= 0;
        if (m->sub_on)
            m->sub_id = stream_number(args[7], 0xBD80);
    }
    if (chapter < 1)
        chapter = 1;
    mlog(m, "menu: play title %d chapter %d audio %d subtitle %d", playlist, chapter, m->audio_id,
         m->sub_on ? m->sub_id : -1);
    m->p.play(m->p.user, playlist, chapter, m->audio_id, m->sub_on ? m->sub_id : -1);
}

NATIVE(mp_stop)
{
    menu *m = user;
    menu_status st;
    (void)vm; (void)self; (void)args; (void)argc; (void)ret;
    m->p.status(m->p.user, &st);
    if (st.playing) {
        m->resume_playlist = st.playlist;
        m->resume_chapter = st.chapter;
    }
    m->p.stop(m->p.user);
}

NATIVE(mp_resume)
{
    menu *m = user;
    (void)vm; (void)self; (void)args; (void)argc; (void)ret;
    m->p.resume(m->p.user);
}

NATIVE(mp_status)
{
    menu *m = user;
    menu_status st;
    (void)self; (void)args; (void)argc;
    m->p.status(m->p.user, &st);
    *ret = vs_new_object(vm);
    /* playStatus: 0 stopped, 1 paused, 2 playing (as the scripts compare it) */
    vs_set(vm, *ret, vs_intern(vm, "playStatus"), vs_num(!st.playing ? 0 : st.paused ? 1 : 2));
    vs_set(vm, *ret, vs_intern(vm, "playListNumber"), vs_num(st.playlist));
    vs_set(vm, *ret, vs_intern(vm, "chapterNumber"), vs_num(st.chapter));
    vs_set(vm, *ret, vs_intern(vm, "audioNumber"), vs_num(0xBD00 + st.audio));
    vs_set(vm, *ret, vs_intern(vm, "subtitleNumber"), vs_num(st.subtitle >= 0 ? 0xBD80 + st.subtitle : -1));
    vs_set(vm, *ret, vs_intern(vm, "subtitleFlag"), vs_num(st.subtitle >= 0));
}

NATIVE(mp_resume_info)
{
    menu *m = user;
    (void)self; (void)args; (void)argc;
    if (!m->resume_playlist)
        return;               /* undefined: nothing to resume */
    *ret = vs_new_object(vm);
    vs_set(vm, *ret, vs_intern(vm, "playListNumber"), vs_num(m->resume_playlist));
    vs_set(vm, *ret, vs_intern(vm, "chapterNumber"), vs_num(m->resume_chapter));
    vs_set(vm, *ret, vs_intern(vm, "audioNumber"), vs_num(0xBD00 + m->audio_id));
    vs_set(vm, *ret, vs_intern(vm, "subtitleNumber"), vs_num(0xBD80 + m->sub_id));
    vs_set(vm, *ret, vs_intern(vm, "subtitleFlag"), vs_num(m->sub_on));
}

NATIVE(mp_change_resume)
{
    menu *m = user;
    (void)vm; (void)self; (void)ret;
    /* changeResumeInfo(-1, audio, -1, subtitle, subtitle on) */
    if (argc >= 5) {
        if (vs_number(args[1]) >= 0)
            m->audio_id = stream_number(args[1], 0xBD00);
        if (vs_number(args[3]) >= 0)
            m->sub_id = stream_number(args[3], 0xBD80);
        m->sub_on = vs_truthy(args[4]);
    }
}

NATIVE(mp_audio)
{
    menu *m = user;
    (void)vm; (void)self; (void)ret;
    m->audio_id = stream_number(arg(args, argc, 0), 0xBD00);
    if (m->audio_id >= 0)
        m->p.set_audio(m->p.user, m->audio_id);
}

NATIVE(mp_subtitle)
{
    menu *m = user;
    (void)vm; (void)self; (void)ret;
    m->sub_id = stream_number(arg(args, argc, 0), 0xBD80);
    if (m->sub_on && m->sub_id >= 0)
        m->p.set_subtitle(m->p.user, m->sub_id);
}

NATIVE(mp_sub_on)
{
    menu *m = user;
    (void)vm; (void)self; (void)args; (void)argc; (void)ret;
    m->sub_on = 1;
    m->p.set_subtitle(m->p.user, m->sub_id >= 0 ? m->sub_id : 0);
}

NATIVE(mp_sub_off)
{
    menu *m = user;
    (void)vm; (void)self; (void)args; (void)argc; (void)ret;
    m->sub_on = 0;
    m->p.set_subtitle(m->p.user, -1);
}

NATIVE(mp_set_pos)
{
    menu *m = user;
    (void)vm; (void)self; (void)ret;
    m->win[0] = argf(args, argc, 0, 0);
    m->win[1] = argf(args, argc, 1, 0);
    m->win_set = 1;
    m->p.set_window(m->p.user, m->win[0], m->win[1], m->win[2], m->win[3]);
}

NATIVE(mp_set_size)
{
    menu *m = user;
    (void)vm; (void)self; (void)ret;
    m->win[2] = argf(args, argc, 0, 0);
    m->win[3] = argf(args, argc, 1, 0);
    m->p.set_window(m->p.user, m->win[0], m->win[1], m->win[2], m->win[3]);
}

static int movieplayer_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    static const struct { const char *name; vs_native fn; } methods[] = {
        {"play", mp_play}, {"stop", mp_stop}, {"resume", mp_resume}, {"getPlayerStatus", mp_status},
        {"getResumeInfo", mp_resume_info}, {"changeResumeInfo", mp_change_resume},
        {"changeAudioStream", mp_audio}, {"changeSubtitleStream", mp_subtitle}, {"turnOnSubtitle", mp_sub_on},
        {"turnOffSubtitle", mp_sub_off}, {"setPos", mp_set_pos}, {"setSize", mp_set_size}};
    menu *m = host;

    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
        if (strcmp(key, methods[i].name) == 0) {
            *out = vs_new_native(vm, methods[i].fn, m);
            return 1;
        }
    if (strcmp(key, "languageCode") == 0 || strcmp(key, "audioLanguageCode") == 0 ||
        strcmp(key, "subtitleLanguageCode") == 0) {
        *out = vs_string(vm, m->language);
        return 1;
    }
    return 0;
}

/* ---- controller ---- */

NATIVE(c_change_resource)
{
    menu *m = user;
    char buf[32];
    (void)self;
    snprintf(m->pending, sizeof(m->pending), "%s", vs_text(arg(args, argc, 0), buf, sizeof(buf)));
    *ret = vs_bool(1);
    (void)vm;
}

NATIVE(c_end)
{
    menu *m = user;
    (void)vm; (void)self; (void)args; (void)argc; (void)ret;
    m->ended = 1;
}

NATIVE(c_get_user)
{
    menu *m = user;
    (void)self; (void)args; (void)argc;
    if (!m->user_count)
        return;
    *ret = vs_new_array(vm, m->user_count);
    for (int i = 0; i < m->user_count; i++) {
        const user_value *u = &m->user[i];
        vs_set_index(vm, *ret, i, u->type == VS_NUM ? vs_num(u->n) : u->type == VS_BOOL ? vs_bool((int)u->n)
                                  : u->type == VS_STR ? vs_string(vm, u->s) : vs_undef());
    }
}

NATIVE(c_set_user)
{
    menu *m = user;
    vs_value a = arg(args, argc, 0);
    (void)self; (void)ret;
    m->user_count = vs_length(a) < MAX_USERDATA ? vs_length(a) : MAX_USERDATA;
    for (int i = 0; i < m->user_count; i++) {
        vs_value v = vs_get_index(vm, a, i);
        char buf[64];
        m->user[i].type = v.type == VS_NUM || v.type == VS_BOOL || v.type == VS_STR ? v.type : VS_UNDEF;
        m->user[i].n = vs_number(v);
        snprintf(m->user[i].s, sizeof(m->user[i].s), "%s", vs_text(v, buf, sizeof(buf)));
    }
}

static int controller_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    static const struct { const char *name; vs_native fn; } methods[] = {
        {"changeResource", c_change_resource}, {"end", c_end}, {"getUserData", c_get_user},
        {"setUserData", c_set_user}};

    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
        if (strcmp(key, methods[i].name) == 0) {
            *out = vs_new_native(vm, methods[i].fn, host);
            return 1;
        }
    return 0;
}

/* ---- timer, resource ---- */

NATIVE(t_set)
{
    menu *m = user;
    (void)self;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (!m->timers[i].used) {
            mtimer *t = &m->timers[i];
            t->used = 1;
            t->id = ++m->next_timer;
            t->fn = vs_hold(vm, arg(args, argc, 0));
            t->self = -1;
            t->interval = (uint32_t)argf(args, argc, 1, 0);
            t->due = m->now + (uint64_t)t->interval * 1000;
            *ret = vs_num(t->id);
            return;
        }
    mlog(m, "menu: too many timers");
}

NATIVE(t_clear)
{
    menu *m = user;
    int id = (int)vs_number(arg(args, argc, 0));
    (void)self; (void)ret;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (m->timers[i].used && m->timers[i].id == id) {
            vs_release(vm, m->timers[i].fn);
            m->timers[i].used = 0;
        }
}

static int timer_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    if (strcmp(key, "setInterval") == 0) {
        *out = vs_new_native(vm, t_set, host);
        return 1;
    }
    if (strcmp(key, "clearInterval") == 0) {
        *out = vs_new_native(vm, t_clear, host);
        return 1;
    }
    return 0;
}

static ref table_refs[3];

static int resource_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    menu *m = host;
    int kind = strcmp(key, "pagetable") == 0 ? 0 : strcmp(key, "animtable") == 0 ? 1
             : strcmp(key, "texturetable") == 0 ? 2 : -1;

    if (kind < 0)
        return 0;
    table_refs[kind].m = m;
    table_refs[kind].index = kind;
    *out = vs_new_host(vm, &table_class, &table_refs[kind]);
    return 1;
}

static const vs_class movieplayer_class = {"movieplayer", movieplayer_get, NULL, NULL};
static const vs_class controller_class = {"controller", controller_get, NULL, NULL};
static const vs_class timer_class = {"timer", timer_get, NULL, NULL};
static const vs_class resource_class = {"resource", resource_get, NULL, NULL};

NATIVE(g_writeln)
{
    char buf[64];
    (void)vm; (void)self; (void)ret;
    mlog((menu *)user, "menu: script says \"%s\"", vs_text(arg(args, argc, 0), buf, sizeof(buf)));
}

/* ---- resources ---- */

static void free_res(res *r)
{
    if (!r)
        return;
    vs_destroy(r->vm);
    for (int i = 0; r->rgba && i < r->r.image_count; i++)
        free(r->rgba[i]);
    free(r->rgba);
    free(r->img_w);
    free(r->img_h);
    free(r->w);
    free(r->wref);
    free(r->aref);
    free(r->iref);
    free(r->focus);
    rco_free(&r->r);
    free(r->file);
    free(r);
}

static res *load_res(menu *m, const char *name)
{
    char path[64];
    uint32_t size;
    res *r = calloc(1, sizeof(res));
    vs_value widgets;

    if (!r)
        return NULL;
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(path, sizeof(path), "UMD_VIDEO/RESOURCE/%s.RCO", name);
    if ((r->file = m->p.read_file(m->p.user, path, &size)) == NULL || rco_parse(&r->r, r->file, size) < 0 ||
        !r->r.script) {
        mlog(m, "menu: cannot load %s", path);
        free_res(r);
        return NULL;
    }
    r->w = calloc((size_t)r->r.object_count + 1, sizeof(wstate));
    r->wref = calloc((size_t)r->r.object_count + 1, sizeof(ref));
    r->focus = calloc((size_t)r->r.object_count + 1, sizeof(int));
    r->aref = calloc((size_t)r->r.anim_count + 1, sizeof(ref));
    r->iref = calloc((size_t)r->r.image_count + 1, sizeof(ref));
    r->rgba = calloc((size_t)r->r.image_count + 1, sizeof(uint8_t *));
    r->img_w = calloc((size_t)r->r.image_count + 1, sizeof(int));
    r->img_h = calloc((size_t)r->r.image_count + 1, sizeof(int));
    r->vm = vs_create(r->r.script, r->r.script_size, vm_log, m);
    if (!r->w || !r->wref || !r->focus || !r->aref || !r->iref || !r->rgba || !r->img_w || !r->img_h || !r->vm) {
        free_res(r);
        return NULL;
    }
    r->active = -1;
    widgets = vs_new_array(r->vm, r->r.object_count);
    for (int i = 0; i < r->r.object_count; i++) {
        r->wref[i].m = m;
        r->wref[i].index = i;
        r->focus[i] = -1;
        reset_widget(r, i);
        vs_set_index(r->vm, widgets, i, vs_new_host(r->vm, &widget_class, &r->wref[i]));
    }
    for (int i = 0; i < r->r.anim_count; i++) {
        r->aref[i].m = m;
        r->aref[i].index = i;
    }
    for (int i = 0; i < r->r.image_count; i++) {
        r->iref[i].m = m;
        r->iref[i].index = i;
    }
    vs_set_global(r->vm, "\001widgets", widgets);
    vs_set_global(r->vm, "movieplayer", vs_new_host(r->vm, &movieplayer_class, m));
    vs_set_global(r->vm, "controller", vs_new_host(r->vm, &controller_class, m));
    vs_set_global(r->vm, "timer", vs_new_host(r->vm, &timer_class, m));
    vs_set_global(r->vm, "resource", vs_new_host(r->vm, &resource_class, m));
    vs_set_global(r->vm, "writeln", vs_new_native(r->vm, g_writeln, m));
    vs_set_global(r->vm, "write", vs_new_native(r->vm, g_writeln, m));
    mlog(m, "menu: loaded %s (%d widgets, %d animations, %d images)", name, r->r.object_count, r->r.anim_count,
         r->r.image_count);
    return r;
}

/* Switches to another RCO: the scripts keep their state in the user data. */
static void apply_pending(menu *m)
{
    char name[16];
    res *r;

    while (m->pending[0]) {
        snprintf(name, sizeof(name), "%s", m->pending);
        m->pending[0] = 0;
        if ((r = load_res(m, name)) == NULL)
            return;
        for (int i = 0; i < MAX_TIMERS; i++)
            m->timers[i].used = 0;
        memset(m->tweens, 0, sizeof(m->tweens));
        memset(m->runs, 0, sizeof(m->runs));
        m->lock = 0;
        free_res(m->res);
        m->res = r;
        m->dirty = 1;
        if (vs_run_main(r->vm) < 0)
            mlog(m, "menu: %s top level failed", name);
        call_value(m, vs_get(r->vm, vs_global(r->vm, "controller"), vs_intern(r->vm, "onChangeResource")),
                   vs_global(r->vm, "controller"), NULL, 0);
    }
}

/* ---- public ---- */

menu *menu_open(const menu_player *player, const char *language)
{
    menu *m = calloc(1, sizeof(menu));

    if (!m)
        return NULL;
    m->p = *player;
    snprintf(m->language, sizeof(m->language), "%s", language && language[0] ? language : "en");
    m->sub_id = -1;
    if ((m->res = load_res(m, START_RCO)) == NULL) {
        free(m);
        return NULL;
    }
    if (vs_run_main(m->res->vm) < 0)
        mlog(m, "menu: top level failed");
    apply_pending(m);
    m->dirty = 1;
    return m;
}

void menu_close(menu *m)
{
    if (!m)
        return;
    free_res(m->res);
    free(m);
}

static void call_handler(menu *m, const char *object, const char *handler, const vs_value *args, int argc)
{
    vsmx *vm = m->res->vm;
    vs_value obj = vs_global(vm, object);
    vs_value fn = vs_get(vm, obj, vs_intern(vm, handler));

    if (!vs_is_function(fn)) {
        mlog(m, "menu: no %s.%s", object, handler);
        return;
    }
    call_value(m, fn, obj, args, argc);
}

void menu_start(menu *m)
{
    call_handler(m, "controller", "onAutoPlay", NULL, 0);
}

void menu_playlist_ended(menu *m, int playlist)
{
    vsmx *vm = m->res->vm;
    vs_value info = vs_new_object(vm);

    vs_set(vm, info, vs_intern(vm, "playListNumber"), vs_num(playlist));
    call_handler(m, "movieplayer", "onPlayListEnd", &info, 1);
}

void menu_chapter_changed(menu *m, int chapter)
{
    vsmx *vm = m->res->vm;
    vs_value info = vs_new_object(vm);
    menu_status st;

    m->p.status(m->p.user, &st);
    vs_set(vm, info, vs_intern(vm, "chapterNumber"), vs_num(chapter));
    vs_set(vm, info, vs_intern(vm, "playListNumber"), vs_num(st.playlist));
    call_handler(m, "movieplayer", "onChapter", &info, 1);
}

void menu_menu_button(menu *m)
{
    call_handler(m, "controller", "onMenu", NULL, 0);
}

int menu_update(menu *m, uint32_t pressed, uint64_t now_us)
{
    m->now = now_us;
    for (int i = 0; i < MAX_TIMERS; i++) {
        mtimer *t = &m->timers[i];
        if (t->used && now_us >= t->due) {
            vs_value fn = vs_held(m->res->vm, t->fn);
            res *r = m->res;
            t->due = now_us + (uint64_t)(t->interval ? t->interval : 1) * 1000;
            call_value(m, fn, vs_undef(), NULL, 0);
            if (m->res != r)
                break;
        }
    }
    step_anims(m);
    step_tweens(m);
    do_input(m, pressed);
    return !m->ended;
}

void menu_debug(const menu *m)
{
    const res *r = m->res;

    mlog((menu *)m, "menu: %s, %d pages open, active %s, lock %d", r->name, r->page_count,
         r->active >= 0 && r->r.objects[r->active].name ? r->r.objects[r->active].name : "-", m->lock);
    for (int p = 0; p < r->page_count; p++)
        for (int i = 0; i < r->r.object_count; i++)
            if (page_of(r, i) == r->pages[p])
                mlog((menu *)m, "menu:   %-22s pos %6.1f %6.1f %6.1f scale %.2f rot %.2f %.2f %.2f rgba %.2f %.2f %.2f %.2f image %d", r->r.objects[i].name ? r->r.objects[i].name : "?",
                     r->w[i].pos[0], r->w[i].pos[1], r->w[i].pos[2], r->w[i].scale[0], r->w[i].rot[0], r->w[i].rot[1], r->w[i].rot[2], r->w[i].color[0], r->w[i].color[1], r->w[i].color[2], r->w[i].color[3], r->w[i].image);
}

int menu_dirty(const menu *m)
{
    return m->dirty;
}

int menu_has_focus(const menu *m)
{
    return m->res->active >= 0;
}

/* ---- drawing ---- */

static const uint8_t *image_rgba(res *r, int i, int *w, int *h)
{
    if (i < 0 || i >= r->r.image_count)
        return NULL;
    if (!r->rgba[i] && r->r.images[i].data) {
        const uint8_t *data = r->r.images[i].data;
        uint32_t size = r->r.images[i].size;
        int iw, ih, jpeg = size > 2 && data[0] == 0xFF && data[1] == 0xD8, ok;
        uint8_t *work = NULL;
        if ((jpeg ? jpeg_info(data, size, &iw, &ih) : png_info(data, size, &iw, &ih)) < 0 || iw <= 0 || ih <= 0 ||
            iw > 1024 || ih > 1024)
            return NULL;
        r->rgba[i] = malloc((size_t)iw * ih * 4);
        if (!jpeg)
            work = malloc((size_t)(iw * 8 + 1) * ih);
        ok = r->rgba[i] && (jpeg ? jpeg_decode(data, size, r->rgba[i], iw * 4) == 0
                                 : work && png_decode(data, size, r->rgba[i], iw * 4, work, (uint32_t)(iw * 8 + 1) * ih) == 0);
        if (ok) {
            r->img_w[i] = iw;
            r->img_h[i] = ih;
        } else {
            free(r->rgba[i]);
            r->rgba[i] = NULL;
        }
        free(work);
    }
    *w = r->img_w[i];
    *h = r->img_h[i];
    return r->rgba[i];
}

/* A widget's place in the scene: origin t and axes (columns of l), in menu units, y up. */
typedef struct xform {
    float t[3];
    float l[3][3];            /* l[row][column] */
    float color[4];
} xform;

static void mat_mul(float out[3][3], float a[3][3], float b[3][3])
{
    float r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(out, r, sizeof(r));
}

/* Rotation about x, then y, then z (setRotate(x, y, z), radians), then scale. */
static void local_matrix(float out[3][3], const float *rot, const float *scale)
{
    float cx = cosf(rot[0]), sx = sinf(rot[0]), cy = cosf(rot[1]), sy = sinf(rot[1]);
    float cz = cosf(rot[2]), sz = sinf(rot[2]);
    float rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    float rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    float s[3][3] = {{scale[0], 0, 0}, {0, scale[1], 0}, {0, 0, scale[2]}};

    mat_mul(out, rz, ry);
    mat_mul(out, out, rx);
    mat_mul(out, out, s);
}

/* Draws an image as the parallelogram centre c + u * ex + v * ey (u, v in -0.5..0.5, screen
   pixels), multiplied by color, over what is there (straight alpha). */
static int *blit_box;          /* grows to cover every blit of a menu_draw */

static void blit(canvas *c, const uint8_t *src, int sw, int sh, const float *crop, float cx, float cy,
                 const float *ex, const float *ey, const float *color)
{
    float det = ex[0] * ey[1] - ex[1] * ey[0];
    float ext = fabsf(ex[0]) / 2 + fabsf(ey[0]) / 2, eyt = fabsf(ex[1]) / 2 + fabsf(ey[1]) / 2;
    int x0 = (int)floorf(cx - ext), x1 = (int)ceilf(cx + ext), y0 = (int)floorf(cy - eyt), y1 = (int)ceilf(cy + eyt);
    /* crop: the visible part as x, y, w, h fractions of the image; the rest is not drawn */
    float u0 = crop ? crop[0] : 0, v0 = crop ? crop[1] : 0, u1 = crop ? crop[0] + crop[2] : 1;
    float v1 = crop ? crop[1] + crop[3] : 1;

    if (fabsf(det) < 0.5f || color[3] <= 0.004f)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (blit_box && x0 < x1 && y0 < y1) {
        if (x0 < blit_box[0]) blit_box[0] = x0;
        if (y0 < blit_box[1]) blit_box[1] = y0;
        if (x1 > blit_box[2]) blit_box[2] = x1;
        if (y1 > blit_box[3]) blit_box[3] = y1;
    }
    for (int y = y0; y < y1; y++) {
        uint8_t *row = c->rgba + (size_t)y * c->stride;
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float u = (dx * ey[1] - dy * ey[0]) / det + 0.5f, v = (ex[0] * dy - ex[1] * dx) / det + 0.5f;
            const uint8_t *s;
            uint8_t *d;
            float a, da, oa;
            int sx, sy;
            if (u < u0 || u >= u1 || v < v0 || v >= v1 || u < 0 || u >= 1 || v < 0 || v >= 1)
                continue;
            sx = (int)(u * (float)sw);
            sy = (int)(v * (float)sh);
            if (sx >= sw || sy >= sh)
                continue;
            s = src + ((size_t)sy * sw + sx) * 4;
            a = s[3] / 255.0f * color[3];
            if (a <= 0.004f)
                continue;
            d = row + x * 4;
            da = d[3] / 255.0f;
            oa = a + da * (1 - a);
            for (int k = 0; k < 3; k++) {
                float sv = s[k] * color[k];
                d[k] = (uint8_t)((sv * a + d[k] * da * (1 - a)) / oa + 0.5f);
            }
            d[3] = (uint8_t)(oa * 255 + 0.5f);
        }
    }
}

static void draw_tree(res *r, canvas *c, int i, const xform *parent)
{
    const wstate *w = &r->w[i];
    float lm[3][3];
    xform x;

    for (int k = 0; k < 4; k++)
        x.color[k] = parent->color[k] * w->color[k];
    if (x.color[3] <= 0.004f || w->hidden)
        return;
    /* Position in the parent's frame, then this widget's own rotation and scale. */
    for (int k = 0; k < 3; k++)
        x.t[k] = parent->t[k] + parent->l[k][0] * w->pos[0] + parent->l[k][1] * w->pos[1] + parent->l[k][2] * w->pos[2];
    local_matrix(lm, w->rot, w->scale);
    mat_mul(x.l, (float (*)[3])parent->l, lm);
    if (w->image >= 0 && r->r.objects[i].kind != RCO_PAGE && r->r.objects[i].kind != RCO_GROUP) {
        int iw, ih;
        const uint8_t *img = image_rgba(r, w->image, &iw, &ih);
        /* Facing away (the plane's normal points into the screen): not drawn. */
        if (img && x.l[2][2] > -0.001f) {
            float sxu = (float)c->w / UNITS_W, syu = (float)c->h / UNITS_H;
            float width = w->size[0] > 0 ? w->size[0] : (float)iw, height = w->size[1] > 0 ? w->size[1] : (float)ih;
            /* Image axes on screen: x right, and y down (the image's rows go down). */
            float ex[2] = {x.l[0][0] * width * sxu, -x.l[1][0] * width * syu};
            float ey[2] = {-x.l[0][1] * height * sxu, x.l[1][1] * height * syu};
            float cx = (UNITS_W / 2 + x.t[0]) * sxu, cy = (UNITS_H / 2 - x.t[1]) * syu;
            blit(c, img, iw, ih, w->has_crop ? w->crop : NULL, cx, cy, ex, ey, x.color);
        }
    }
    for (int ch = r->r.objects[i].first_child; ch >= 0; ch = r->r.objects[ch].next_sibling)
        draw_tree(r, c, ch, &x);
}

void menu_draw(menu *m, canvas *c)
{
    res *r = m->res;
    xform root;

    memset(&root, 0, sizeof(root));
    for (int k = 0; k < 3; k++)
        root.l[k][k] = 1;
    for (int k = 0; k < 4; k++)
        root.color[k] = 1;
    /* Clear only what the last picture covered; the rest of the canvas is still clear. */
    if (m->drawn[2] > m->drawn[0])
        canvas_rect(c, m->drawn[0], m->drawn[1], m->drawn[2] - m->drawn[0], m->drawn[3] - m->drawn[1], 0);
    else if (!m->drawn[3])
        canvas_clear(c);      /* first picture: the canvas may hold anything */
    m->drawn[0] = c->w;
    m->drawn[1] = c->h;
    m->drawn[2] = m->drawn[3] = 0;
    blit_box = m->drawn;
    for (int p = 0; p < r->page_count; p++)
        draw_tree(r, c, r->pages[p], &root);
    blit_box = NULL;
    if (m->drawn[2] <= m->drawn[0])
        m->drawn[3] = 1;      /* nothing drawn, but the canvas is known clear */
    m->dirty = 0;
}
