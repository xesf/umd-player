#include "menu.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/jpeg.h"
#include "../core/png.h"
#include "../core/rco.h"
#include "../core/umdv.h"
#include "../core/vag.h"
#include "../core/vsmx.h"
#include "../platform/platform.h"

#define MAX_TWEENS   256
#define MAX_RUNS     128
#define MAX_TIMERS   32
#define MAX_PAGES    16
#define MAX_USERDATA 64
#define FILL_MAX     16          /* images this narrow or short are fills and strips: they stretch */
#define USERDATA_EMPTY 32           /* getUserData before anything was saved */
#define UNITS_W      854.0f           /* menu coordinates: a 16:9 screen 480 units high */
#define UNITS_H      480.0f
#define PERSP_D      507.668f         /* the menu camera's distance: twice The Boondock Saints' z */


enum { P_POS, P_COLOR, P_SCALE, P_ROT, P_CROP, P_SIZE, P_FADE };

/* What the script can change on a widget. */
typedef struct wstate {
    float pos[3], color[4], size[3], scale[3], rot[3], crop[4];
    int image, has_crop;
    int hidden;               /* widget.visible = false: it and its children are not drawn */
    float fade;               /* fade steps (RCO step 6): an opacity apart from the colour's alpha */
    float view_rot[3];        /* while a rotation turns: the orientation drawn, the short way */
    int view_rot_on;
    int script_image;         /* the script set its texture */
} wstate;

typedef struct tween {
    int active, widget, prop, n;
    int halfway;              /* a turn logged half done ("rot:" lines) */
    float from[4], to[4], dur;
    uint64_t start;
} tween;

typedef struct anim_run {
    int active, anim, step;
    int locked;               /* a lock step ran: input waits until an unlock or the end */
    int target;               /* anim.play(widget): the widget for steps that name none */
    uint64_t wait_until;
    uint64_t started;         /* the menu clock when it was played */
} anim_run;

#define TIMER_ARGS 8

typedef struct mtimer {
    int used, id, fn, self;   /* fn and self: vs_hold handles */
    int args[TIMER_ARGS], argc;   /* setInterval(fn, ms, ...): what fn is called with, held */
    uint32_t interval;
    uint64_t due;
} mtimer;

typedef struct ref {
    menu *m;
    int index;                /* widget, animation or image index */
} ref;

typedef struct user_value {
    int type;                 /* VS_NUM, VS_STR, VS_BOOL, or VS_UNDEF */
    int is_int;
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
    ref *wref, *aref, *iref, *sref;
    int16_t **snd_pcm;        /* each sound decoded to 48 kHz stereo on its first play */
    int *snd_frames;
    uint8_t **rgba;           /* decoded images, on first use */
    int *img_w, *img_h;
    uint8_t *img_fill;        /* one colour, or a one-way gradient: a fill that stretches to its widget */
    uint8_t *img_clear;       /* some pixels fully transparent: a frame or an overlay, not a card */
    uint8_t *img_plain;       /* a filled black box: four fifths solid, every visible pixel black */
    uint8_t *own_size;        /* per widget: draw the image at its own size (same_size_labels) */
    float *label_k;           /* per widget: or this many times its size, as its neighbours (0: no) */
    uint8_t **occ;            /* per image: its 8x8 blocks with a visible pixel (see blit) */
    uint32_t *img_id;         /* per image: unique while decoded, for the GPU's copy of it */
    int pages[MAX_PAGES], page_count;   /* open pages, in drawing order */
    int activated;                       /* the page last activated with no focus, and when */
    int active_left;                     /* active only because the page above it closed */
    uint64_t activated_at;
    int active;               /* page that takes input, or -1 */
    int focus_page;           /* page of the button the script focused last, or -1 (input_page) */
    int *focus;               /* per page: the focused button, or -1 */
    int focused;              /* the button that had the focus-in last, on any page, or -1 */
} res;

struct menu {
    menu_player p;
    char language[3];
    res *res;
    char pending[16];         /* a resource change asked for during a script call */
    int ended;
    tween tweens[MAX_TWEENS];
    int poll_widget, poll_prop, poll_count;   /* get_values: one moving value read again and again */
    uint64_t poll_now;
    anim_run runs[MAX_RUNS];
    mtimer timers[MAX_TIMERS];
    int next_timer;
    int lock;                 /* animations holding an input lock */
    uint64_t now;
    uint32_t repeat_button;   /* a held direction, sent again from repeat_at on, as on a PSP */
    uint64_t repeat_at;
    int dirty;
    user_value user[MAX_USERDATA];
    int user_count;
    int resume_playlist, resume_chapter;   /* for getResumeInfo */
    uint8_t resumable[UMDV_MAX_TITLES + 1];  /* by 1-based title: a main video, a place to resume */
    int interactive;          /* no title is a main video: an interactive disc (UMD-PG) */
    int menu_mode;            /* the title playing is a menu video: play(0, 1, ...) */
    int audio_id, sub_id, sub_on;
    float win[4];
    int win_set;
    int drawn_once;           /* menu_draw has drawn the whole canvas once */
    int touch_down, touch_x0, touch_y0, touch_x, touch_y;   /* menu_touch: where a finger went down, and is */
    int depth;                /* script calls in progress */
    int settling;
    struct { const char *event; int widget; res *from; } queue[64];
    int queued;
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

/* Turning menus (Feel the Noise's drum, The Legend of Zorro's wheel) in the log, lines that
   start "rot:": the keys, every turn a script or an animation asks for, what getRotate
   answers, and where each turn ends, to compare with a PSP. Moves are logged for widgets in
   a turned group. */
static const char *wname(const menu *m, int i)
{
    const char *n = i >= 0 && i < m->res->r.object_count ? m->res->r.objects[i].name : NULL;
    return n ? n : "?";
}

static int in_turned_group(const res *r, int i)
{
    for (; i >= 0; i = r->r.objects[i].parent)
        if (r->w[i].rot[0] != 0 || r->w[i].rot[1] != 0 || r->w[i].rot[2] != 0)
            return 1;
    return 0;
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
    w->script_image = 0;
    w->has_crop = 0;
    w->hidden = 0;
    w->fade = 1;
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
    default: *n = 1; return &w->fade;
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
    for (int i = 0; i < MAX_TWEENS; i++)     /* a new step replaces a tween on the same property */
        if (m->tweens[i].active && m->tweens[i].widget == widget && m->tweens[i].prop == prop) {
            t = &m->tweens[i];
            break;
        }
    if (dur <= 0) {           /* instant; a tween left running would overwrite it when it ends */
        if (t)
            t->active = 0;
        if (prop == P_ROT)
            r->w[widget].view_rot_on = 0;
        memcpy(cur, to, (size_t)n * sizeof(float));
        m->dirty = 1;
        return;
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
    t->halfway = 0;
}

/* ---- rotations as orientations ---- */

/* The quaternion (w, x, y, z) of the rotation Y, then X, then Z (see local_matrix). */
static void euler_quat(const float *r, float q[4])
{
    float cx = cosf(r[0] / 2), sx = sinf(r[0] / 2), cy = cosf(r[1] / 2), sy = sinf(r[1] / 2);
    float cz = cosf(r[2] / 2), sz = sinf(r[2] / 2);
    /* qy * qx * qz */
    float w = cy * cx, x = cy * sx, y = sy * cx, z = -sy * sx;
    q[0] = w * cz - z * sz;
    q[1] = x * cz + y * sz;
    q[2] = y * cz - x * sz;
    q[3] = z * cz + w * sz;
}

/* Back to angles for Y, X, Z, from the quaternion's matrix. */
static void quat_euler(const float q[4], float *r)
{
    float w = q[0], x = q[1], y = q[2], z = q[3];
    float m02 = 2 * (x * z + w * y), m12 = 2 * (y * z - w * x), m22 = 1 - 2 * (x * x + y * y);
    float m10 = 2 * (x * y + w * z), m11 = 1 - 2 * (x * x + z * z);
    float m00 = 1 - 2 * (y * y + z * z), m01 = 2 * (x * y - w * z);
    r[0] = asinf(-m12 > 1 ? 1 : -m12 < -1 ? -1 : -m12);
    if (fabsf(m12) < 0.9999f) {
        r[1] = atan2f(m02, m22);
        r[2] = atan2f(m10, m11);
    } else {                  /* straight up or down: Y and Z turn about the same axis */
        r[1] = 0;
        r[2] = atan2f(-m01, m00);
    }
}

/* The rotation k of the way from a to b along the shorter arc. */
static void rotation_between(const float *a, const float *b, float k, float *out)
{
    float qa[4], qb[4], q[4], dot = 0, len = 0;

    euler_quat(a, qa);
    euler_quat(b, qb);
    for (int i = 0; i < 4; i++)
        dot += qa[i] * qb[i];
    if (dot < 0) {            /* q and -q are the same rotation: take the nearer one */
        for (int i = 0; i < 4; i++)
            qb[i] = -qb[i];
        dot = -dot;
    }
    if (dot > 0.9995f) {
        for (int i = 0; i < 4; i++)
            q[i] = qa[i] + (qb[i] - qa[i]) * k;
    } else {
        float th = acosf(dot), s = sinf(th);
        float fa = sinf((1 - k) * th) / s, fb = sinf(k * th) / s;
        for (int i = 0; i < 4; i++)
            q[i] = qa[i] * fa + qb[i] * fb;
    }
    for (int i = 0; i < 4; i++)
        len += q[i] * q[i];
    len = sqrtf(len);
    for (int i = 0; i < 4; i++)
        q[i] /= len;
    quat_euler(q, out);
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
        if (t->prop == P_ROT && !t->active)
            mlog(m, "rot: %s turned to %.3f %.3f %.3f", wname(m, t->widget), cur[0], cur[1], cur[2]);
        /* Rotations are drawn turning as orientations, the short way round: the wheels of The
           Legend of Zorro and Christmas With the Kranks step between angles such as
           (pi/3, -pi, -pi) and (pi/3, pi, pi), the same orientation, and turning each angle
           on its own spun the items over. The scripts still read the angles moving
           (getRotate), and build their next steps from them. */
        if (t->prop == P_ROT && t->n == 3) {
            wstate *w = &m->res->w[t->widget];
            w->view_rot_on = k < 1;
            if (k < 1)
                rotation_between(t->from, t->to, k, w->view_rot);
            if (k >= 0.5f && k < 1 && !t->halfway) {
                t->halfway = 1;
                mlog(m, "rot: %s half way: angles %.3f %.3f %.3f, drawn as %.3f %.3f %.3f", wname(m, t->widget),
                     cur[0], cur[1], cur[2], w->view_rot[0], w->view_rot[1], w->view_rot[2]);
            }
        }
        m->dirty = 1;
    }
}

/* ---- script calls ---- */

static void apply_pending(menu *m);

/* Calls an event handler named in the RCO ("script:/main/onPush_btnRootMenu"). */
static void settle(menu *m);

/* Every script call goes through here. Resource changes and widget events wait until the
   outermost call returns: a page opened by a script gets its onInit after that script ends,
   as on the PSP, and a new resource's top level has defined its handlers by then. */
static void call_value(menu *m, vs_value fn, vs_value self, const vs_value *args, int argc)
{
    m->depth++;
    vs_call(m->res->vm, fn, self, args, argc, NULL);
    m->depth--;
    if (m->depth == 0)
        settle(m);
}

/* Queues an event handler named in the RCO ("script:/main/onPush_btnRootMenu"). */
static void call_event(menu *m, const char *event, int widget)
{
    if (!event)
        return;
    if (m->queued < (int)(sizeof(m->queue) / sizeof(m->queue[0]))) {
        m->queue[m->queued].event = event;
        m->queue[m->queued].widget = widget;
        m->queue[m->queued].from = m->res;
        m->queued++;
    } else {
        mlog(m, "menu: too many events at once");
    }
    if (m->depth == 0)
        settle(m);
}

static void run_event(menu *m, const char *event, int widget)
{
    const char *name = strrchr(event, '/');
    vs_value w = vs_undef();
    res *r = m->res;

    name = name ? name + 1 : event;
    if (getenv("MENU_TRACE"))
        mlog(m, "menu: event %s", name);
    if (widget >= 0)
        w = vs_get_index(r->vm, vs_global(r->vm, "\001widgets"), widget);
    if (!vs_is_function(vs_global(r->vm, name))) {
        mlog(m, "menu: no handler %s", name);
        return;
    }
    call_value(m, vs_global(r->vm, name), w, &w, 1);
}

static void settle(menu *m)
{
    if (m->settling)
        return;
    m->settling = 1;
    for (int guard = 0; guard < 256 && (m->pending[0] || m->queued); guard++) {
        if (m->pending[0]) {
            apply_pending(m);
            continue;
        }
        {
            const char *event = m->queue[0].event;
            int widget = m->queue[0].widget;
            res *from = m->queue[0].from;
            memmove(&m->queue[0], &m->queue[1], (size_t)(m->queued - 1) * sizeof(m->queue[0]));
            m->queued--;
            if (from == m->res)          /* events of an unloaded resource are dropped */
                run_event(m, event, widget);
        }
    }
    m->settling = 0;
}

static void play_anim(menu *m, int anim, int target);

static vs_value widget_value(menu *m, int i);

/* A handler the script set on the widget (Nav.onPush = function ...). Returns 1 if it ran. */
static int script_handler(menu *m, int widget, const char *name)
{
    vsmx *vm = m->res->vm;
    vs_value w = widget_value(m, widget), fn;

    if (widget < 0)
        return 0;
    fn = vs_get(vm, w, vs_intern(vm, name));
    if (!vs_is_function(fn))
        return 0;
    if (getenv("MENU_TRACE"))
        mlog(m, "menu: %s.%s", m->res->r.objects[widget].name ? m->res->r.objects[widget].name : "?", name);
    call_value(m, fn, w, &w, 1);
    return 1;
}

/* A widget action: a handler the script set on the widget wins; otherwise the RCO's script
   handler, or an animation played on the widget. Returns 0 if there was nothing to do. */
static int do_action_named(menu *m, const rco_action *a, int widget, const char *name)
{
    /* The event can load another resource and free the one a points into. */
    const char *event = a->event;
    int anim = a->anim;
    res *r = m->res;

    if (name && script_handler(m, widget, name))
        return 1;
    if (event)
        call_event(m, event, widget);
    if (anim >= 0 && m->res == r)
        play_anim(m, anim, widget);
    return event || anim >= 0;
}

static void do_action(menu *m, const rco_action *a, int widget)
{
    do_action_named(m, a, widget, NULL);
}

/* ---- animations ---- */

static void play_anim(menu *m, int anim, int target)
{
    anim_run *run = NULL;

    if (getenv("MENU_TRACE"))
        mlog(m, "menu: play animation %s on %s", m->res->r.anims[anim].name ? m->res->r.anims[anim].name : "?",
             target >= 0 && m->res->r.objects[target].name ? m->res->r.objects[target].name : "-");

    /* The same animation can run on several widgets at once (a list hides each item), but a
       widget runs one animation at a time: playing one stops the others on it. Freddy vs.
       Jason stops its looping button fades by playing "killAnim", a bare delay, on the button.
       Animations played in the same moment all run: Fawlty Towers plays its episode page's
       transition on the button and then moves the focus, whose focus-out animation on that
       button stopped the transition before it opened the page or ended its input lock. */
    for (int i = 0; i < MAX_RUNS; i++) {
        if (!m->runs[i].active || m->runs[i].target != target || target < 0)
            continue;
        if (m->runs[i].anim == anim)
            run = &m->runs[i];
        else if (m->runs[i].started < m->now)
            m->runs[i].active = 0;
    }
    for (int i = 0; !run && i < MAX_RUNS; i++)
        if (!m->runs[i].active)
            run = &m->runs[i];
    if (!run)
        return;
    /* An animation that plays itself again keeps its lock through the restart: House on Haunted
       Hill locks keys for its warning with a lock, a delay of 1 s and the same animation again,
       and a press in the frame between the loops reached the menu button. */
    if (!(run->active && run->anim == anim))
        run->locked = 0;
    run->active = 1;
    run->anim = anim;
    run->target = target;
    run->step = 0;
    run->wait_until = m->now;
    run->started = m->now;
}

/* Runs an animation's steps that are due; 0 if an event changed the resource. */
static int step_run(menu *m, anim_run *run)
{
    res *r = m->res;
    while (run->active && m->now >= run->wait_until) {
        const rco_anim *a = &r->r.anims[run->anim];
        const rco_step *s;
        int obj;
        if (run->step >= a->step_count) {
            run->active = 0;      /* an animation's lock ends with it */
            run->locked = 0;
            break;
        }
        s = &r->r.steps[a->first_step + run->step++];
        obj = s->object >= 0 ? s->object : run->target;
        switch (s->kind) {
        case RCO_STEP_MOVE: start_tween(m, obj, P_POS, s->value, 3, s->duration); break;
        case RCO_STEP_COLOR: start_tween(m, obj, P_COLOR, s->value, 4, s->duration); break;
        case RCO_STEP_ROTATE:
            mlog(m, "rot: %s step turn %.3f %.3f %.3f in %.0f ms (animation %s)", wname(m, obj), s->value[0],
                 s->value[1], s->value[2], s->duration, r->r.anims[run->anim].name ? r->r.anims[run->anim].name : "?");
            start_tween(m, obj, P_ROT, s->value, 3, s->duration);
            break;
        case RCO_STEP_SCALE: start_tween(m, obj, P_SCALE, s->value, 3, s->duration); break;
        case RCO_STEP_FADE: start_tween(m, obj, P_FADE, s->value, 1, s->duration); break;
        case RCO_STEP_CROP: start_tween(m, obj, P_CROP, s->value, 4, s->duration); break;
        case RCO_STEP_DELAY: run->wait_until = m->now + (uint64_t)(s->duration * 1000); break;
        case RCO_STEP_LOCK: run->locked = 1; break;
        case RCO_STEP_UNLOCK: run->locked = 0; break;
        case RCO_STEP_EVENT:
            if (s->anim >= 0) {
                /* Another animation, on the same widget. Playing this one again restarts
                   it; from the next frame, so a loop without a delay cannot spin. */
                int again = s->anim == run->anim;
                play_anim(m, s->anim, run->target);
                if (again)
                    run->wait_until = m->now + 1;
            }
            if (!s->event)
                break;
            /* "this" is the widget the animation runs on (postBlink switches on this.name) */
            call_event(m, s->event, run->target);
            if (m->res != r)
                return 0; /* the event changed the resource */
            /* The handler played an animation on this widget again: it starts next frame.
               Battlestar Galactica's menu loops this way, an event step whose handler
               plays the animation again, and spun inside one frame. */
            if (run->active && run->step == 0 && run->wait_until <= m->now)
                run->wait_until = m->now + 1;
            break;
        default: break;
        }
    }
    return 1;
}

/* 1 if an animation that has not begun changes the widget before its first wait. */
static int first_steps_touch(const menu *m, const anim_run *run, int index)
{
    const rco_anim *a = &m->res->r.anims[run->anim];
    if (run->target == index)
        return 1;
    for (int i = 0; i < a->step_count; i++) {
        const rco_step *s = &m->res->r.steps[a->first_step + i];
        if ((s->kind == RCO_STEP_DELAY && s->duration > 0) || s->kind == RCO_STEP_EVENT)
            return 0;
        if ((s->object >= 0 ? s->object : run->target) == index && s->kind != RCO_STEP_DELAY &&
            s->kind != RCO_STEP_LOCK && s->kind != RCO_STEP_UNLOCK)
            return 1;
    }
    return 0;
}

static void step_anims(menu *m)
{
    for (int i = 0; i < MAX_RUNS; i++)
        if (!step_run(m, &m->runs[i]))
            return;
}

/* ---- focus and input ---- */

static int page_open(const res *r, int page)
{
    for (int p = 0; p < r->page_count; p++)
        if (r->pages[p] == page)
            return 1;
    return 0;
}

/* The screen has one focus: a button taking it on another page (a submenu opening, the menu
   it returns to) takes it from the button that had it, which gets its focus-out. That button
   stays its page's focus, for the page's return. High Tension keeps the item it opens large as
   the submenu's heading by skipping the next focus-out, the one its page's buttons taking the
   focus give; without it the item it returned to skipped the shrink of the next move. */
static void take_focus(menu *m, int widget)
{
    res *r = m->res;
    int old = r->focused, page = page_of(r, old);

    r->focused = widget;
    if (old < 0 || old == widget || page < 0 || page == page_of(r, widget) || !page_open(r, page) || r->focus[page] != old)
        return;
    do_action_named(m, &r->r.objects[old].action[RCO_BTN_FOCUS_OUT], old, "onFocusOut");
}

static void set_focus(menu *m, int widget)
{
    res *r = m->res;
    int page = page_of(r, widget), old;

    if (page < 0)
        return;
    old = r->focus[page];
    if (getenv("MENU_TRACE"))
        mlog(m, "menu: focus %s -> %s", old >= 0 && r->r.objects[old].name ? r->r.objects[old].name : "-",
             r->r.objects[widget].name ? r->r.objects[widget].name : "?");
    if (old == widget)
        return;
    r->focus[page] = widget;
    m->dirty = 1;
    if (old >= 0) {
        if (r->focused == old)
            r->focused = -1;
        do_action_named(m, &r->r.objects[old].action[RCO_BTN_FOCUS_OUT], old, "onFocusOut");
    }
    if (m->res == r)
        take_focus(m, widget);
    if (m->res == r)
        do_action_named(m, &r->r.objects[widget].action[RCO_BTN_FOCUS_IN], widget, "onFocusIn");
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

/* A page that has settled with buttons and no focus gets its first button focused, with that
   button's focus-in action: Tenjho Tenge and Shorts Play highlight an item only that way, and
   showed none until the first key, which then also moved past it. Not over a playing movie,
   where Valiant leaves a page of hidden buttons active. */
/* The page that takes input: the active one, or when it has no buttons the page of the
   button the script focused last, if still open. Vacancy and Underworld Evolution focus Nav on
   pageRootMenu and then activate p_PLMM, a page of pictures. */
static int input_page(const res *r)
{
    if (r->active >= 0 && first_button(r, r->active) >= 0)
        return r->active;
    for (int p = 0; r->focus_page >= 0 && p < r->page_count; p++)
        if (r->pages[p] == r->focus_page)
            return r->focus_page;
    return r->active;
}

static void auto_focus(menu *m)
{
    res *r = m->res;
    int page = r->active, first;
    menu_status st;

    /* Nor a page left active by the close of the one above it, until the script activates it:
       Escaflowne closes its main menu as its set-up transition starts, with the set-up page open
       for later, and highlighted the first set-up choice through the transition. */
    if (page < 0 || m->lock || r->focus[page] >= 0 || r->active_left || (first = first_button(r, page)) < 0)
        return;
    m->p.status(m->p.user, &st);
    if (st.playing && !m->menu_mode)
        return;
    set_focus(m, first);
}

static void do_input(menu *m, uint32_t pressed)
{
    static const struct { uint32_t button; int action; const char *name; } dirs[] = {
        {PLAT_BTN_LEFT, RCO_BTN_LEFT, "onFocusLeft"}, {PLAT_BTN_RIGHT, RCO_BTN_RIGHT, "onFocusRight"},
        {PLAT_BTN_UP, RCO_BTN_UP, "onFocusUp"}, {PLAT_BTN_DOWN, RCO_BTN_DOWN, "onFocusDown"}};
    res *r = m->res;
    int page = input_page(r), focus;

    if (page < 0 || m->lock || !pressed)
        return;
    focus = r->focus[page];
    if (focus < 0 && (focus = first_button(r, page)) >= 0)
        set_focus(m, focus);
    if (m->res != r)
        return;
    if (pressed & PLAT_BTN_CIRCLE) {
        /* The focused widget's script handler, then the page's. A page with neither (Saw's
           title descriptions) leaves Back to the disc's menu event, as the menu button. */
        if (!script_handler(m, focus, "onCancel") &&
            !do_action_named(m, &r->r.objects[page].action[RCO_PAGE_CANCEL], page, "onCancel"))
            menu_menu_button(m);
        return;
    }
    /* Square is the PSP's menu key in a menu: Little Britain's first series says "press
       square for the main menu" and goes back in controller.onMenu. */
    if (pressed & PLAT_BTN_SQUARE) {
        menu_menu_button(m);
        return;
    }
    if (focus < 0)
        return;
    if (pressed & PLAT_BTN_CROSS) {
        do_action_named(m, &r->r.objects[focus].action[RCO_BTN_PUSH], focus, "onPush");
        return;
    }
    for (unsigned i = 0; i < 4; i++)
        if (pressed & dirs[i].button) {
            const rco_action *a = &r->r.objects[focus].action[dirs[i].action];
            mlog(m, "rot: key %s on %s (page %s)", dirs[i].name + 7, wname(m, focus), wname(m, page));
            if (script_handler(m, focus, dirs[i].name))
                ;
            else if (a->object >= 0)
                set_focus(m, a->object);
            else
                do_action(m, a, focus);
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

static const uint8_t *image_rgba(res *r, int i, int *w, int *h);

/* Decodes the images of a page before it shows, so animations do not stall on PNG decoding. */
static void preload(res *r, int i)
{
    int w, h;

    if (r->w[i].image >= 0)
        image_rgba(r, r->w[i].image, &w, &h);
    for (int c = r->r.objects[i].first_child; c >= 0; c = r->r.objects[c].next_sibling)
        preload(r, c);
}

/* The widgets' own onInit, as their page opens: an animation on the widget or a script
   handler. Underworld Evolution draws the right and lower halves of its ornaments by turning
   left-half images by pi (animFlipH, animFlipV); Toy Story starts its aliens wiggling. */
static void init_tree(menu *m, res *r, int i)
{
    for (int c = r->r.objects[i].first_child; c >= 0 && m->res == r; c = r->r.objects[c].next_sibling) {
        do_action(m, &r->r.objects[c].init, c);
        init_tree(m, r, c);
    }
}

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
        preload(r, rf->index);
        r->focus[rf->index] = -1;
        r->pages[r->page_count++] = rf->index;
        init_tree(rf->m, r, rf->index);
        if (rf->m->res == r)
            do_action(rf->m, &r->r.objects[rf->index].action[RCO_PAGE_INIT], rf->index);
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
    if (r->active == rf->index) {
        r->active = r->page_count ? r->pages[r->page_count - 1] : -1;
        r->active_left = 1;
    }
    /* Animations on the closed page's widgets end with it, and their locks: Jackass locks
       input on its intro page with a lock step and a delay of 15 days, and opens the menu page
       after it. A Force of One pulses its focused item with an animation that replays itself;
       left running while the page was closed, the item it had entered kept pulsing beside the
       one focused on return. */
    for (int i = 0; i < MAX_RUNS; i++)
        if (rf->m->runs[i].active && rf->m->runs[i].target >= 0 && page_of(r, rf->m->runs[i].target) == rf->index) {
            rf->m->runs[i].locked = 0;
            rf->m->runs[i].active = 0;
        }
    rf->m->dirty = 1;
}

NATIVE(w_activate)
{
    ref *rf = user;
    res *r = rf->m->res;
    (void)vm; (void)args; (void)argc;
    int was = r->active, focus = r->focus[rf->index];
    *ret = self;
    r->active = rf->index;
    r->active_left = 0;
    do_action(rf->m, &r->r.objects[rf->index].action[RCO_PAGE_ACTIVATE], rf->index);
    /* A page made active again gives its focused button its focus-in: First Blood moves its
       crosshair back to the chosen item there on return from its scene and subtitle pages. */
    if (was != rf->index && focus >= 0 && rf->m->res == r && r->focus[rf->index] == focus) {
        take_focus(rf->m, focus);
        if (rf->m->res == r)
            do_action_named(rf->m, &r->r.objects[focus].action[RCO_BTN_FOCUS_IN], focus, "onFocusIn");
    }
    if (focus < 0) {
        r->activated = rf->index;
        r->activated_at = rf->m->now;
    }
}

NATIVE(w_set_focus)
{
    ref *rf = user;
    res *r = rf->m->res;
    /* Only buttons take the focus: a plane or a group passes it to its first button, and one
       without a button keeps the focus where it is. Superbad focuses rootPlane.children[0],
       its background picture, which left the menu taking no input. */
    int target = first_button(r, rf->index), page = target >= 0 ? page_of(r, target) : -1;
    (void)vm; (void)args; (void)argc;
    *ret = self;
    if (target < 0)
        return;
    r->focus_page = page;
    /* A page activated just now with no focus has given it to its first button, with a
       focus-in: First Blood ignores that one on its subtitle page, and moves its crosshair to
       the chosen subtitle on the focus-in of the button it focuses next. Focusing the first
       button itself gives one focus-in: First Blood does not wait for a second one then. */
    if (page >= 0 && page == r->activated && r->activated_at == rf->m->now && r->focus[page] < 0 &&
        page == r->active && first_button(r, page) != target) {
        set_focus(rf->m, first_button(r, page));
        if (rf->m->res != r)
            return;
    }
    /* The script focusing the widget that has the focus still gets its focus-in: Austin
       Powers restarts its pulse and records the selected item there on return from a
       submenu. */
    if (page >= 0 && r->focus[page] == target) {
        take_focus(rf->m, target);
        if (rf->m->res == r)
            do_action_named(rf->m, &r->r.objects[target].action[RCO_BTN_FOCUS_IN], target, "onFocusIn");
    } else
        set_focus(rf->m, target);
}

/* setPos(x, y, z) sets at once; animPos(x, y, z, ms) moves over time: the duration is the
   last argument, and missing values keep their current value. */
static void set_values(ref *rf, int prop, const vs_value *args, int argc, int timed, int n)
{
    float v[4], dur = 0;
    float *cur;
    int count;

    /* The time follows the values: animColor(r, g, b, a, ms), animPos(x, y, z, ms). Given no
       more arguments than values, there is no time: First Descent calls animColor(1, 1, 1,
       250) to show its chosen scene's name, an alpha of 250 (full) at once. */
    if (timed && argc > n) {
        dur = (float)vs_number(args[n]);
        if (!(dur > 0))               /* undefined (NaN) or negative: at once. Robots passes an unset variable */
            dur = 0;
        argc = n;
    } else if (timed) {
        timed = 0;
    }
    /* An animation played on the widget just before has started, as on a PSP, so a value
       set now wins over its first steps: Starship Troopers (eng) plays animSSChapFocusIn on
       the chosen scene (colour 1 over 0.5 s), then sets that scene's colour to grey. The same
       holds for one played on its group that moves it: Appleseed places its subtitle labels
       with animSUBS_SetMoveIn on the page, then sets where they go. */
    for (int i = 0; i < MAX_RUNS; i++) {
        anim_run *run = &rf->m->runs[i];
        if (run->active && run->step == 0 && run->started == rf->m->now && first_steps_touch(rf->m, run, rf->index) &&
            !step_run(rf->m, run))
            return;
    }
    cur = prop_ptr(&rf->m->res->w[rf->index], prop, &count);
    if (n > count)
        n = count;
    for (int i = 0; i < n; i++) {
        v[i] = argf(args, argc, i, cur[i]);
        if (prop == P_COLOR)          /* colours are fractions: 250 is full */
            v[i] = v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i];
    }
    if (prop == P_ROT || (prop == P_POS && in_turned_group(rf->m->res, rf->index)))
        mlog(rf->m, "rot: %s.%s%s(%.3f %.3f %.3f) in %.0f ms, from %.3f %.3f %.3f", wname(rf->m, rf->index),
             timed ? "anim" : "set", prop == P_ROT ? "Rotate" : "Pos", v[0], n > 1 ? v[1] : 0, n > 2 ? v[2] : 0, dur,
             cur[0], count > 1 ? cur[1] : 0, count > 2 ? cur[2] : 0);
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

/* A script that waits for a tween by reading the value in a loop sees it arrive at once, as
   the PSP's clock runs on while a script spins: Austin Powers in Goldmember plays its
   pictogram with animPos and waits for getPos to reach the end. */
#define POLL_FINISH 1000

static void get_values(vsmx *vm, ref *rf, int prop, vs_value *ret)
{
    menu *m = rf->m;
    int n;
    float *v = prop_ptr(&m->res->w[rf->index], prop, &n);

    if (m->poll_now == m->now && m->poll_widget == rf->index && m->poll_prop == prop) {
        if (++m->poll_count >= POLL_FINISH) {   /* each tween started since, too: sleep after sleep */
            for (int i = 0; i < MAX_TWEENS; i++) {
                tween *t = &m->tweens[i];
                if (t->active && t->widget == rf->index && t->prop == prop)
                    t->start = m->now - (uint64_t)(t->dur * 1000.0f) - 1;
            }
            step_tweens(m);
        }
    } else {
        m->poll_now = m->now;
        m->poll_widget = rf->index;
        m->poll_prop = prop;
        m->poll_count = 0;
    }
    if (prop == P_ROT && n == 3) {
        /* The PSP reads a turn back from its matrix (Y, then X, then Z), X within -pi/2..pi/2:
           a turn of 2pi/3 about X reads (pi/3, -pi, -pi), and (pi, pi, pi), no turn at all,
           reads (0, 0, 0). Feel the Noise, To Die For and Memoirs of a Geisha keep Y and Z
           from it to aim their drum's next turn; given the stored angles they turned to the
           wrong item on Up. Products keep their signed zeros as on the PSP (0 * cos(2pi/3) is
           -0, and atan2 of -0 is -pi). With X at a quarter turn the matrix cannot tell Y
           from Z, and the PSP reads both as 0: a drum of four, Deuce Bigalow's,
           stands at -pi/2 after one step, and read as (-pi, -pi) from the cosine's float
           error (-4e-8), its next step, aimed with them, turned the drum back instead of on. */
        float sx = sinf(v[0]), cx = cosf(v[0]), sy = sinf(v[1]), cy = cosf(v[1]);
        float sz = sinf(v[2]), cz = cosf(v[2]);
        float e[3] = {asinf(sx < -1 ? -1 : sx > 1 ? 1 : sx), atan2f(sy * cx, cy * cx), atan2f(cx * sz, cx * cz)};
        if (fabsf(cx) < 1e-6f)
            e[1] = e[2] = 0;
        mlog(m, "rot: %s.getRotate() = %.3f %.3f %.3f (stored %.3f %.3f %.3f)", wname(m, rf->index), e[0], e[1], e[2],
             v[0], v[1], v[2]);
        *ret = array3(vm, e, 3);
        return;
    }
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
        r->w[rf->index].script_image = 1;
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
        *out = vs_int(n);
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

/* A sound decoded to 48 kHz stereo, as the player's audio: a mono sound plays on both sides. */
static int16_t *sound_pcm(res *r, int i, int *frames)
{
    const rco_sound *sd = &r->r.sounds[i];
    int16_t *ch[2] = {NULL, NULL}, *out = NULL;
    int count[2] = {0, 0}, rate = 0, n;

    if (r->snd_pcm[i]) {
        *frames = r->snd_frames[i];
        return r->snd_pcm[i];
    }
    for (int c = 0; c < sd->channels; c++)
        if (sd->data[c])
            ch[c] = vag_decode(sd->data[c], sd->size[c], &count[c], &rate);
    if (!ch[1]) {
        ch[1] = ch[0];
        count[1] = count[0];
    }
    n = count[0] < count[1] ? count[0] : count[1];
    if (ch[0] && rate > 0 && n > 0) {
        int len = (int)((int64_t)n * 48000 / rate);
        if (len > 0 && (out = malloc((size_t)len * 4)) != NULL)
            for (int k = 0; k < len; k++) {   /* linear between samples: 16 to 48 kHz on the discs */
                int64_t pos = (int64_t)k * rate * 256 / 48000;
                int at = (int)(pos >> 8), fr = (int)(pos & 255), nx = at + 1 < n ? at + 1 : at;
                out[k * 2] = (int16_t)((ch[0][at] * (256 - fr) + ch[0][nx] * fr) >> 8);
                out[k * 2 + 1] = (int16_t)((ch[1][at] * (256 - fr) + ch[1][nx] * fr) >> 8);
            }
        r->snd_frames[i] = out ? len : 0;
    }
    if (ch[1] != ch[0])
        free(ch[1]);
    free(ch[0]);
    r->snd_pcm[i] = out;
    *frames = r->snd_frames[i];
    return out;
}

/* resource.soundtable.<name>.play() and stop(): menu sound effects, mixed over the video's
   sound. Kill Bill Vol. 2 plays sndOne as the selection moves and sndTwo on a press. A name
   the table lacks plays nothing, so the script carries on. */
NATIVE(sound_play)
{
    ref *rf = user;
    int frames = 0;
    const int16_t *pcm;
    (void)vm; (void)self; (void)args; (void)argc; (void)ret;
    if (rf->index < 0 || !rf->m->p.sound || (pcm = sound_pcm(rf->m->res, rf->index, &frames)) == NULL)
        return;
    rf->m->p.sound(rf->m->p.user, pcm, frames);
}

NATIVE(sound_stop)
{
    ref *rf = user;
    (void)vm; (void)self; (void)args; (void)argc; (void)ret;
    if (rf->index >= 0 && rf->m->p.sound)
        rf->m->p.sound(rf->m->p.user, NULL, 0);
}

static int sound_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    if (strcmp(key, "play") == 0)
        *out = vs_new_native(vm, sound_play, host);
    else if (strcmp(key, "stop") == 0)
        *out = vs_new_native(vm, sound_stop, host);
    else
        return 0;
    return 1;
}

static const vs_class sound_class = {"sound", sound_get, NULL, NULL, NULL};

/* resource.pagetable, resource.animtable, resource.texturetable, resource.soundtable: the host
   is a table kind. */
static int table_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    ref *rf = host;
    res *r = rf->m->res;
    int i;

    (void)vm;
    *out = vs_undef();
    if (rf->index == 3) {
        i = rco_find_sound(&r->r, key);
        *out = vs_new_host(vm, &sound_class, &r->sref[i >= 0 ? i : r->r.sound_count]);
        return 1;
    }
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

static const char *widget_type(void *host)
{
    ref *rf = host;
    switch (rf->m->res->r.objects[rf->index].kind) {
    case RCO_PAGE: return "Page";
    case RCO_PLANE: return "Plane";
    case RCO_GROUP: return "Group";
    case RCO_BUTTON: return "Button";
    default: return "Object";
    }
}

static const vs_class widget_class = {"widget", widget_get, widget_set, NULL, widget_type};
static const vs_class children_class = {"children", children_get, NULL, NULL, NULL};
static const vs_class anim_class = {"anim", anim_get, NULL, NULL, NULL};
static const vs_class table_class = {"table", table_get, NULL, NULL, NULL};
static const vs_class texture_class = {"texture", NULL, NULL, NULL, NULL};

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
    m->ended = 0;             /* the disc goes on after controller.end(): Vacancy calls it in
                                 onPlayListEnd before playing its menu */
    if (argc < 3) {
        /* play(0, 0) names no title. Speed calls it near the end of its warnings and Harry
           Potter after its credits; the title plays on to its end, where onPlayListEnd decides
           what follows. */
        mlog(m, "menu: play with no title: playback carries on");
        return;
    }
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
    if (playlist < 1) {
        /* play(-1): UMD-PG games ask for this under a choice window; the picture holds. */
        mlog(m, "menu: play title %d: holding the picture", playlist);
        return;
    }
    mlog(m, "menu: play title %d chapter %d audio %d subtitle %d", playlist, chapter, m->audio_id,
         m->sub_on ? m->sub_id : -1);
    {
        menu_status st;
        m->p.status(m->p.user, &st);
        if (st.playing && !m->menu_mode && st.playlist != playlist && menu_title_resumable(m, st.playlist)) {   /* as a stop: getResumeInfo */
            m->resume_playlist = st.playlist;
            m->resume_chapter = st.chapter;
        }
    }
    m->menu_mode = vs_truthy(arg(args, argc, 1));
    m->p.play(m->p.user, playlist, chapter, m->audio_id, m->sub_on ? m->sub_id : -1, m->menu_mode,
              args[0].type == VS_NUM && vs_number(args[0]) == 1);
}

NATIVE(mp_stop)
{
    menu *m = user;
    menu_status st;
    (void)vm; (void)self; (void)args; (void)argc; (void)ret;
    m->p.status(m->p.user, &st);
    if (st.playing && !m->menu_mode && menu_title_resumable(m, st.playlist)) {   /* nor a menu video or a clip */
        m->resume_playlist = st.playlist;
        m->resume_chapter = st.chapter;
    }
    m->p.stop(m->p.user);
}

NATIVE(mp_resume)
{
    menu *m = user;
    (void)vm; (void)self; (void)args; (void)argc;
    m->menu_mode = 0;
    *ret = vs_int(m->p.resume(m->p.user));     /* scripts test for 0 before playing from the start */
}

NATIVE(mp_status)
{
    menu *m = user;
    menu_status st;
    (void)self; (void)args; (void)argc;
    m->p.status(m->p.user, &st);
    *ret = vs_new_object(vm);
    /* playStatus: 1 playing; 2 paused, or stopped with a place to resume; 0 nothing. Harry
       Potter logs "successfully playing video" on 1 and Speed waits for 1 before it uncovers
       the movie; 2012 stops the movie and waits for 2 before resume(), and Freddy vs. Jason
       counts 1 or 2 as a title in progress. */
    vs_set(vm, *ret, vs_intern(vm, "playStatus"),
           vs_int(st.playing ? (st.paused ? 2 : 1) : m->resume_playlist && menu_title_resumable(m, m->resume_playlist) ? 2 : 0));
    vs_set(vm, *ret, vs_intern(vm, "playListNumber"), vs_int(st.playlist));
    vs_set(vm, *ret, vs_intern(vm, "chapterNumber"), vs_int(st.chapter));
    /* 1 while the menu has the screen: a menu video, or nothing playing. Pirates of the
       Caribbean stops the player before changing resource and waits for menuMode 1. */
    vs_set(vm, *ret, vs_intern(vm, "menuMode"), vs_int(!st.playing || m->menu_mode));
    /* 0 is normal speed, anything else paused or fast play; the scripts only test for 0.
       Summer Days shows its password at normal speed and hides it when that changes. */
    vs_set(vm, *ret, vs_intern(vm, "playSpeed"), vs_int(st.playing && st.paused ? -1 : 0));
    vs_set(vm, *ret, vs_intern(vm, "audioNumber"), vs_int(0xBD00 + st.audio));
    vs_set(vm, *ret, vs_intern(vm, "subtitleNumber"), vs_int(st.subtitle >= 0 ? 0xBD80 + st.subtitle : -1));
    vs_set(vm, *ret, vs_intern(vm, "subtitleFlag"), vs_int(st.subtitle >= 0));
}

NATIVE(mp_resume_info)
{
    menu *m = user;
    (void)self; (void)args; (void)argc;
    if (!m->resume_playlist || !menu_title_resumable(m, m->resume_playlist))
        return;               /* undefined: nothing to resume */
    *ret = vs_new_object(vm);
    vs_set(vm, *ret, vs_intern(vm, "playListNumber"), vs_int(m->resume_playlist));
    vs_set(vm, *ret, vs_intern(vm, "chapterNumber"), vs_int(m->resume_chapter));
    vs_set(vm, *ret, vs_intern(vm, "audioNumber"), vs_int(0xBD00 + m->audio_id));
    vs_set(vm, *ret, vs_intern(vm, "subtitleNumber"), vs_int(0xBD80 + m->sub_id));
    vs_set(vm, *ret, vs_intern(vm, "subtitleFlag"), vs_int(m->sub_on));
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

/* The video window: full screen (854x480 at the centre) until setPos or setSize. */
NATIVE(mp_get_size)
{
    menu *m = user;
    float v[2] = {m->win[2] > 0 ? m->win[2] : 854, m->win[3] > 0 ? m->win[3] : 480};
    (void)self; (void)args; (void)argc;
    *ret = array3(vm, v, 2);
}

NATIVE(mp_get_pos)
{
    menu *m = user;
    (void)self; (void)args; (void)argc;
    *ret = array3(vm, m->win, 2);
}

static int movieplayer_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    static const struct { const char *name; vs_native fn; } methods[] = {
        {"play", mp_play}, {"stop", mp_stop}, {"resume", mp_resume}, {"getPlayerStatus", mp_status},
        {"getResumeInfo", mp_resume_info}, {"changeResumeInfo", mp_change_resume},
        {"changeAudioStream", mp_audio}, {"changeSubtitleStream", mp_subtitle}, {"turnOnSubtitle", mp_sub_on},
        {"turnOffSubtitle", mp_sub_off}, {"setPos", mp_set_pos}, {"setSize", mp_set_size},
        {"getPos", mp_get_pos}, {"getSize", mp_get_size}};
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
    if (!m->user_count) {
        /* Nothing saved yet: an array of zeros, which the scripts fill in and save
           (Summer Days writes its start position into it before changing resource). */
        *ret = vs_new_array(vm, USERDATA_EMPTY);
        for (int i = 0; i < USERDATA_EMPTY; i++)
            vs_set_index(vm, *ret, i, vs_int(0));
        return;
    }
    *ret = vs_new_array(vm, m->user_count);
    for (int i = 0; i < m->user_count; i++) {
        const user_value *u = &m->user[i];
        vs_set_index(vm, *ret, i, u->type == VS_NUM ? (u->is_int ? vs_int(u->n) : vs_num(u->n)) : u->type == VS_BOOL ? vs_bool((int)u->n)
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
        m->user[i].is_int = v.is_int;
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
            /* Arguments after the interval go to fn on each call: Breakout's timedCall gets
               its counter and its own slot this way, and stops itself with them. */
            t->argc = argc > 2 ? (argc - 2 < TIMER_ARGS ? argc - 2 : TIMER_ARGS) : 0;
            for (int k = 0; k < t->argc; k++)
                t->args[k] = vs_hold(vm, args[2 + k]);
            *ret = vs_int(t->id);
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
            for (int k = 0; k < m->timers[i].argc; k++)
                vs_release(vm, m->timers[i].args[k]);
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

static ref table_refs[4];

static int resource_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    menu *m = host;
    int kind = strcmp(key, "pagetable") == 0 ? 0 : strcmp(key, "animtable") == 0 ? 1
             : strcmp(key, "texturetable") == 0 ? 2 : strcmp(key, "soundtable") == 0 ? 3 : -1;

    if (kind < 0)
        return 0;
    table_refs[kind].m = m;
    table_refs[kind].index = kind;
    *out = vs_new_host(vm, &table_class, &table_refs[kind]);
    return 1;
}

static const vs_class movieplayer_class = {"movieplayer", movieplayer_get, NULL, NULL, NULL};
static const vs_class controller_class = {"controller", controller_get, NULL, NULL, NULL};
static const vs_class timer_class = {"timer", timer_get, NULL, NULL, NULL};
static const vs_class resource_class = {"resource", resource_get, NULL, NULL, NULL};

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
    for (int i = 0; r->rgba && i <= r->r.image_count; i++)   /* and the fill pixel after them */
        free(r->rgba[i]);
    for (int i = 0; r->occ && i <= r->r.image_count; i++)
        free(r->occ[i]);
    free(r->occ);
    free(r->img_id);
    free(r->rgba);
    free(r->img_w);
    free(r->img_h);
    free(r->img_fill);
    free(r->img_plain);
    free(r->img_clear);
    free(r->own_size);
    free(r->label_k);
    free(r->w);
    free(r->wref);
    free(r->aref);
    free(r->iref);
    for (int i = 0; r->snd_pcm && i < r->r.sound_count; i++)
        free(r->snd_pcm[i]);
    free(r->snd_pcm);
    free(r->snd_frames);
    free(r->sref);
    free(r->focus);
    rco_free(&r->r);
    free(r->file);
    free(r);
}

static void same_size_labels(menu *m, res *r);

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
    r->sref = calloc((size_t)r->r.sound_count + 1, sizeof(ref));
    r->snd_pcm = calloc((size_t)r->r.sound_count + 1, sizeof(int16_t *));
    r->snd_frames = calloc((size_t)r->r.sound_count + 1, sizeof(int));
    r->rgba = calloc((size_t)r->r.image_count + 1, sizeof(uint8_t *));
    r->img_w = calloc((size_t)r->r.image_count + 1, sizeof(int));
    r->img_h = calloc((size_t)r->r.image_count + 1, sizeof(int));
    r->img_fill = calloc((size_t)r->r.image_count + 1, 1);
    r->img_plain = calloc((size_t)r->r.image_count + 1, 1);
    r->img_clear = calloc((size_t)r->r.image_count + 1, 1);
    r->own_size = calloc((size_t)r->r.object_count + 1, 1);
    r->label_k = calloc((size_t)r->r.object_count + 1, sizeof(float));
    r->occ = calloc((size_t)r->r.image_count + 1, sizeof(uint8_t *));
    r->img_id = calloc((size_t)r->r.image_count + 1, sizeof(uint32_t));
    r->vm = vs_create(r->r.script, r->r.script_size, vm_log, m);
    if (!r->w || !r->wref || !r->focus || !r->aref || !r->iref || !r->sref || !r->snd_pcm || !r->snd_frames || !r->rgba || !r->img_w || !r->img_h || !r->img_fill || !r->img_plain || !r->img_clear ||
        !r->own_size || !r->label_k || !r->occ || !r->img_id || !r->vm) {
        free_res(r);
        return NULL;
    }
    r->active = -1;
    r->focus_page = -1;
    r->focused = -1;
    r->activated = -1;
    same_size_labels(m, r);
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
    for (int i = 0; i <= r->r.sound_count; i++) {   /* and one for a name the table lacks */
        r->sref[i].m = m;
        r->sref[i].index = i < r->r.sound_count ? i : -1;
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
        /* A menu's own video belongs to the resource it played in: 2012 changes to its warning
           cards on Play without stopping its menu video, which went on behind them. A movie
           keeps playing. */
        {
            menu_status st;
            m->p.status(m->p.user, &st);
            if (st.playing && m->menu_mode)
                m->p.stop(m->p.user);
        }
        for (int i = 0; i < MAX_TIMERS; i++)
            m->timers[i].used = 0;
        memset(m->tweens, 0, sizeof(m->tweens));
        memset(m->runs, 0, sizeof(m->runs));
        m->lock = 0;
        free_res(m->res);
        m->res = r;
        m->dirty = 1;
        m->depth++;
        if (vs_run_main(r->vm) < 0)
            mlog(m, "menu: %s top level failed", name);
        m->depth--;
        {
            vs_value fn = vs_get(r->vm, vs_global(r->vm, "controller"), vs_intern(r->vm, "onChangeResource"));
            if (getenv("MENU_TRACE"))
                mlog(m, "menu: %s.onChangeResource is %s", name, vs_is_function(fn) ? "set" : "missing");
            call_value(m, fn, vs_global(r->vm, "controller"), NULL, 0);
        }
    }
}

/* ---- public ---- */

/* The start menus are XX100000.RCO, XX a language: EN on most discs, JA on Japanese ones (JP
   on Face Off). Prefers the system language, then English, then whichever the disc has. */
static const char *start_resource(menu *m)
{
    static const char *const langs[] = {"EN", "JA", "JP", "FR", "DE", "ES", "IT", "NL", "PT", "RU", "KO", "ZH", "CH", "TW"};
    static char name[16];
    char path[64], first[3] = {(char)(m->language[0] & ~0x20), (char)(m->language[1] & ~0x20), 0};
    uint32_t size;

    for (int i = -1; i < (int)(sizeof(langs) / sizeof(langs[0])); i++) {
        uint8_t *data;
        snprintf(name, sizeof(name), "%s100000", i < 0 ? first : langs[i]);
        snprintf(path, sizeof(path), "UMD_VIDEO/RESOURCE/%s.RCO", name);
        if ((data = m->p.read_file(m->p.user, path, &size)) != NULL) {
            free(data);
            return name;
        }
    }
    return "EN100000";
}

/* Main videos are titles of 10 minutes or more, movies and episodes, and titles that play from
   the same clip as one. Logos, warnings, blanks,
   trailers and menu loops are no place to resume, and keeping one sent re-entered discs back
   into a one-second clip. An interactive disc (UMD-PG) has no title that long; there every
   title counts. */
#define MAIN_VIDEO_TICKS (10u * 60 * UMDV_PTS_HZ)

static void find_main_videos(menu *m)
{
    uint32_t size = 0;
    uint8_t *buf = m->p.read_file(m->p.user, "UMD_VIDEO/PLAYLIST.UMD", &size);
    umdv_playlist *pl = buf ? malloc(sizeof(umdv_playlist)) : NULL;
    int any = 0;

    memset(m->resumable, 1, sizeof(m->resumable));
    m->interactive = 1;
    if (pl && umdv_playlist_parse(buf, size, pl) == 0) {
        for (int t = 0; t < pl->title_count; t++)
            any |= pl->titles[t].end_pts - pl->titles[t].start_pts >= MAIN_VIDEO_TICKS;
        m->interactive = !any;
        for (int t = 0; any && t < pl->title_count; t++)
            m->resumable[t + 1] = pl->titles[t].end_pts - pl->titles[t].start_pts >= MAIN_VIDEO_TICKS;
        /* Titles that play a part of a main video's clip are that video too: Metallica's scene
           selection plays each chapter of the feature as its own short title. */
        for (int t = 0; any && t < pl->title_count; t++)
            for (int u = 0; u < pl->title_count && !m->resumable[t + 1]; u++)
                if (m->resumable[u + 1] && strcmp(pl->titles[t].clip, pl->titles[u].clip) == 0)
                    m->resumable[t + 1] = 1;
    }
    free(pl);
    free(buf);
}

int menu_title_resumable(const menu *m, int playlist)
{
    return playlist >= 1 && playlist <= UMDV_MAX_TITLES && m->resumable[playlist];
}

int menu_interactive(const menu *m)
{
    return m->interactive;
}

menu *menu_open(const menu_player *player, const char *language)
{
    menu *m = calloc(1, sizeof(menu));

    if (!m)
        return NULL;
    m->p = *player;
    find_main_videos(m);
    snprintf(m->language, sizeof(m->language), "%s", language && language[0] ? language : "en");
    m->sub_id = -1;
    if ((m->res = load_res(m, start_resource(m))) == NULL) {
        free(m);
        return NULL;
    }
    m->depth++;
    if (vs_run_main(m->res->vm) < 0)
        mlog(m, "menu: top level failed");
    m->depth--;
    settle(m);
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

int menu_continue(menu *m)
{
    vsmx *vm = m->res->vm;
    vs_value obj = vs_global(vm, "controller");
    vs_value fn = vs_get(vm, obj, vs_intern(vm, "onContinuePlay"));

    if (!vs_is_function(fn))
        return 0;
    call_value(m, fn, obj, NULL, 0);
    return 1;
}

void menu_exit(menu *m)
{
    menu_status st;

    m->p.status(m->p.user, &st);
    if (st.playing && !m->menu_mode && menu_title_resumable(m, st.playlist)) {
        m->resume_playlist = st.playlist;
        m->resume_chapter = st.chapter;
    }
    call_handler(m, "controller", "onExit", NULL, 0);
}

/* "v1 playlist chapter audio subtitle subtitle_on count" and the user data, each value a
   letter and its text: i42, f0.5, b1, s with the string in hex, u for undefined. */
int menu_get_state(const menu *m, char *out, int size)
{
    int len = snprintf(out, (size_t)size, "v1 %d %d %d %d %d %d", m->resume_playlist, m->resume_chapter, m->audio_id,
                       m->sub_id, m->sub_on, m->user_count);

    for (int i = 0; i < m->user_count && len < size; i++) {
        const user_value *u = &m->user[i];
        if (u->type == VS_NUM && u->is_int)
            len += snprintf(out + len, (size_t)(size - len), " i%.0f", u->n);
        else if (u->type == VS_NUM)
            len += snprintf(out + len, (size_t)(size - len), " f%.17g", u->n);
        else if (u->type == VS_BOOL)
            len += snprintf(out + len, (size_t)(size - len), " b%d", u->n != 0);
        else if (u->type == VS_STR) {
            len += snprintf(out + len, (size_t)(size - len), " s");
            for (const char *c = u->s; *c && len < size; c++)
                len += snprintf(out + len, (size_t)(size - len), "%02x", (unsigned char)*c);
        } else
            len += snprintf(out + len, (size_t)(size - len), " u");
    }
    return len < size ? len : 0;
}

void menu_set_state(menu *m, const char *text)
{
    int count, n = 0;
    const char *p;

    if (sscanf(text, "v1 %d %d %d %d %d %d%n", &m->resume_playlist, &m->resume_chapter, &m->audio_id, &m->sub_id,
               &m->sub_on, &count, &n) < 6 || count < 0)
        return;
    p = text + n;
    m->user_count = 0;
    while (m->user_count < count && m->user_count < MAX_USERDATA) {
        user_value *u = &m->user[m->user_count++];
        int len = 0;
        while (*p == ' ')
            p++;
        memset(u, 0, sizeof(*u));
        u->type = VS_UNDEF;
        if (*p == 'i' || *p == 'f' || *p == 'b') {
            u->type = *p == 'b' ? VS_BOOL : VS_NUM;
            u->is_int = *p == 'i';
            u->n = strtod(p + 1, NULL);
        } else if (*p == 's') {
            unsigned c;
            u->type = VS_STR;
            for (p++; len < (int)sizeof(u->s) - 1 && sscanf(p, "%2x", &c) == 1 && p[0] != ' '; p += 2)
                u->s[len++] = (char)c;
        }
        while (*p && *p != ' ')
            p++;
    }
}

void menu_playlist_ended(menu *m, int playlist)
{
    vsmx *vm = m->res->vm;
    vs_value info = vs_new_object(vm);

    vs_set(vm, info, vs_intern(vm, "playListNumber"), vs_int(playlist));
    call_handler(m, "movieplayer", "onPlayListEnd", &info, 1);
}

void menu_chapter_changed(menu *m, int chapter)
{
    vsmx *vm = m->res->vm;
    vs_value info = vs_new_object(vm);
    menu_status st;

    m->p.status(m->p.user, &st);
    vs_set(vm, info, vs_intern(vm, "chapterNumber"), vs_int(chapter));
    vs_set(vm, info, vs_intern(vm, "playListNumber"), vs_int(st.playlist));
    vs_set(vm, info, vs_intern(vm, "mark_name"), vs_string(vm, st.mark_name));
    vs_set(vm, info, vs_intern(vm, "mark_data"), vs_int(st.mark_data));
    call_handler(m, "movieplayer", "onChapter", &info, 1);
}

void menu_mark_reached(menu *m, const char *name, uint32_t data)
{
    vsmx *vm = m->res->vm;
    vs_value info = vs_new_object(vm);
    menu_status st;

    m->p.status(m->p.user, &st);
    mlog(m, "menu: mark %s", name);
    vs_set(vm, info, vs_intern(vm, "playListNumber"), vs_int(st.playlist));
    vs_set(vm, info, vs_intern(vm, "mark_name"), vs_string(vm, name));
    vs_set(vm, info, vs_intern(vm, "mark_data"), vs_int(data));
    call_handler(m, "movieplayer", "onMark", &info, 1);
}

void menu_menu_button(menu *m)
{
    call_handler(m, "controller", "onMenu", NULL, 0);
}

/* A held direction repeats after REPEAT_FIRST, every REPEAT_NEXT: Robots flies its cursor 43
   units a step for as long as the D-pad is held. */
#define REPEAT_FIRST 400000
#define REPEAT_NEXT  120000

static uint32_t repeat_dirs(menu *m, uint32_t pressed, uint32_t held)
{
    const uint32_t dirs = PLAT_BTN_UP | PLAT_BTN_DOWN | PLAT_BTN_LEFT | PLAT_BTN_RIGHT;

    if (pressed & dirs) {
        m->repeat_button = pressed & dirs & (0u - (pressed & dirs));   /* one of them */
        m->repeat_at = m->now + REPEAT_FIRST;
    } else if (m->repeat_button && !(held & m->repeat_button)) {
        m->repeat_button = 0;
    } else if (m->repeat_button && m->now >= m->repeat_at) {
        pressed |= m->repeat_button;
        m->repeat_at = m->now + REPEAT_NEXT;
    }
    return pressed;
}

int menu_update(menu *m, uint32_t pressed, uint32_t held, uint64_t now_us)
{
    m->now = now_us;
    pressed = repeat_dirs(m, pressed, held);
    /* Tweens due now finish before the timers run: Feel the Noise turns its menu drum over
       350 ms and reads the angle back in a 350 ms timer, which caught the turn 95 % done and
       left the drum tilted. */
    step_tweens(m);
    for (int i = 0; i < MAX_TIMERS; i++) {
        mtimer *t = &m->timers[i];
        if (t->used && now_us >= t->due) {
            vs_value fn = vs_held(m->res->vm, t->fn), targs[TIMER_ARGS];
            res *r = m->res;
            int targc = t->argc;
            for (int k = 0; k < targc; k++)
                targs[k] = vs_held(m->res->vm, t->args[k]);
            t->due = now_us + (uint64_t)(t->interval ? t->interval : 1) * 1000;
            call_value(m, fn, vs_undef(), targs, targc);
            if (m->res != r)
                break;
        }
    }
    step_anims(m);
    step_tweens(m);
    /* A lock counts while its animation's widget is on an open page: Jackass and Wildboyz
       lock input for 15 days on a widget of a page they close, sometimes after closing it. */
    m->lock = 0;
    for (int i = 0; i < MAX_RUNS; i++) {
        const anim_run *run = &m->runs[i];
        /* An animation played on no widget locks nothing: Viva La Bam and Wildboyz call
           slowPSP.play(this) from a plain function, which would stop input for 18 s. */
        int page = run->target >= 0 ? page_of(m->res, run->target) : -1, open = 0;
        for (int p = 0; p < m->res->page_count && !open; p++)
            open = m->res->pages[p] == page;
        m->lock += run->active && run->locked && open;
    }
    auto_focus(m);
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
                mlog((menu *)m, "menu:   %-22s pos %6.1f %6.1f %6.1f scale %.2f %.2f %.2f size %.0f %.0f rot %.2f %.2f %.2f rgba %.2f %.2f %.2f %.2f image %d", r->r.objects[i].name ? r->r.objects[i].name : "?",
                     r->w[i].pos[0], r->w[i].pos[1], r->w[i].pos[2], r->w[i].scale[0], r->w[i].scale[1], r->w[i].scale[2], r->w[i].size[0], r->w[i].size[1], r->w[i].rot[0], r->w[i].rot[1], r->w[i].rot[2], r->w[i].color[0], r->w[i].color[1], r->w[i].color[2], r->w[i].color[3], r->w[i].image);
}

int menu_dirty(const menu *m)
{
    return m->dirty;
}

int menu_active_page(const menu *m)
{
    return m->res->active;
}

int menu_locked(const menu *m)
{
    /* Only a lock that is about to end: Jackass, Wildboyz and Viva La Bam park input behind a
       15-day delay that the script ends when onChapter opens the next page, so holding chapter
       events for it would wait for ever. */
    if (m->lock <= 0)
        return 0;
    for (int i = 0; i < MAX_RUNS; i++) {
        const anim_run *run = &m->runs[i];
        if (run->active && run->locked && run->target >= 0 && run->wait_until < m->now + 5000000)
            return 1;
    }
    return 0;
}

int menu_input_locked(const menu *m)
{
    return m->lock > 0;
}

int menu_has_focus(const menu *m)
{
    /* A page without buttons takes no input: Saw leaves pageWaitDummy, a lone picture, active
       while the movie plays, and the player keeps its buttons. */
    int page = input_page(m->res);
    return page >= 0 && first_button(m->res, page) >= 0;
}

/* ---- drawing ---- */

static uint32_t next_image_id;

/* Image index image_count is a white pixel: the picture of a plane with a size and no image,
   drawn in its colour (fill_image). */
static const uint8_t *image_rgba(res *r, int i, int *w, int *h)
{
    if (i < 0 || i > r->r.image_count)
        return NULL;
    if (i == r->r.image_count) {
        if (!r->rgba[i] && (r->rgba[i] = malloc(4)) != NULL && (r->occ[i] = malloc(1)) != NULL) {
            memset(r->rgba[i], 255, 4);
            r->occ[i][0] = 1;
            r->img_fill[i] = 1;
            r->img_id[i] = ++next_image_id;
            r->img_w[i] = r->img_h[i] = 1;
        }
    } else if (!r->rgba[i] && r->r.images[i].data) {
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
            const uint32_t *px = (const uint32_t *)r->rgba[i];
            int rows = 1, cols = 1;
            /* A gradient that changes in one direction only, every row or every column one
               colour, has no shape to keep either: The Da Vinci Code darkens each cryptex
               strip with an 86x286 black gradient in its 58x206 widget, and at its own size
               the five overlapped in a black box. */
            for (int y = 0; y < ih && rows; y++)
                for (int x = 1; x < iw && rows; x++)
                    rows = px[y * iw + x] == px[y * iw];
            for (int y = 1; y < ih && cols; y++)
                for (int x = 0; x < iw && cols; x++)
                    cols = px[y * iw + x] == px[x];
            int plain = 1, solid = 0, clear = 0, dense = 1;
            for (int k = 0; k < iw * ih && !clear; k++)
                clear = r->rgba[i][(size_t)k * 4 + 3] == 0;
            r->img_clear[i] = (uint8_t)clear;
            uint32_t rgb = 0, seen = 0;
            for (int k = 0; k < iw * ih && plain; k++)
                if (r->rgba[i][(size_t)k * 4 + 3]) {
                    solid += r->rgba[i][(size_t)k * 4 + 3] >= 128;
                    dense &= r->rgba[i][(size_t)k * 4 + 3] >= 200;
                    uint32_t c3 = px[k] & 0x00FFFFFFu;   /* little-endian RGBA: the colour bytes */
                    if (!seen)
                        rgb = c3, seen = 1;
                    plain = c3 == rgb;
                }
            /* One colour over the whole image, its edges at most a little softer, is a fill too:
               Escaflowne's 19x19 cyan box with feathered edges highlights its 427x240 scene
               frame, and at its own size it was a small square. */
            r->img_fill[i] = (uint8_t)(rows || cols || (plain && seen && dense && !clear));
            r->img_plain[i] = (uint8_t)(plain && seen && rgb == 0 && solid >= iw * ih * 4 / 5);
            r->img_id[i] = ++next_image_id;
            r->img_w[i] = iw;
            r->img_h[i] = ih;
            /* Which 8x8 blocks have a visible pixel: the rings of The Transporter's dial are
               500x500 images with 3% of them visible, and turning, reading them all missed the
               Vita's caches at every pixel. */
            if ((r->occ[i] = calloc((size_t)((iw + 7) / 8) * ((ih + 7) / 8), 1)) != NULL)
                for (int y = 0; y < ih; y++)
                    for (int x = 0; x < iw; x++)
                        if (r->rgba[i][((size_t)y * iw + x) * 4 + 3])
                            r->occ[i][(y / 8) * ((iw + 7) / 8) + x / 8] = 1;
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

    /* Y, then X, then Z (a vector is turned about Z first). The wheels of Christmas With the
       Kranks and The Legend of Zorro set angles such as (pi/3, pi, pi), which are the X turn
       pi - pi/3 in this order; with Z, Y, X they flipped the wheel over. */
    mat_mul(out, ry, rx);
    mat_mul(out, out, rz);
    mat_mul(out, out, s);
}

/* Draws an image as the parallelogram centre c + u * ex + v * ey (u, v in -0.5..0.5, screen
   pixels), multiplied by color, over what is there (straight alpha). */
/* One image to draw: where it goes on the canvas and how. menu_draw builds the list each
   time, compares it with the last one, and redraws only the region that changed. */
typedef struct sprite {
    int widget;
    const uint8_t *img;
    const uint8_t *occ;       /* the image's visible 8x8 blocks, or NULL */
    int image;                /* index in the resource's images */
    int iw, ih;
    float cx, cy, ex[2], ey[2], color[4], crop[4];
    int has_crop;
    int box[4];               /* x0 y0 x1 y1 on the canvas */
} sprite;

#define MAX_SPRITES 512

static void sprite_box(sprite *s, int cw, int ch)
{
    float hx = fabsf(s->ex[0]) / 2 + fabsf(s->ey[0]) / 2, hy = fabsf(s->ex[1]) / 2 + fabsf(s->ey[1]) / 2;
    s->box[0] = (int)floorf(s->cx - hx);
    s->box[1] = (int)floorf(s->cy - hy);
    s->box[2] = (int)ceilf(s->cx + hx) + 1;
    s->box[3] = (int)ceilf(s->cy + hy) + 1;
    if (s->box[0] < 0) s->box[0] = 0;
    if (s->box[1] < 0) s->box[1] = 0;
    if (s->box[2] > cw) s->box[2] = cw;
    if (s->box[3] > ch) s->box[3] = ch;
}

/* n / d for d in 1..255 without a division, which the Vita's CPU does in software, slowly:
   a multiply by ceil(2^32 / d). Exact for n * (ceil(2^32 / d) * d - 2^32) < 2^32, so for every
   n up to 16 million, more than the blend below needs (255^3). */
static uint32_t recip[256];

static void init_recip(void)
{
    for (uint32_t d = 2; d < 256; d++)
        recip[d] = (uint32_t)((((uint64_t)1 << 32) + d - 1) / d);
}

static inline uint32_t div_small(uint32_t n, uint32_t d)
{
    return d == 1 ? n : (uint32_t)(((uint64_t)n * recip[d]) >> 32);
}

/* Narrows [*k0, *k1) to the steps k where lo <= start + k * step < hi, give or take one: a
   row of a turned image only crosses the image on part of its bounding box. The pixel loop
   still tests each step. Returns 0 when no step is left. */
static int row_span(int32_t start, int32_t step, int32_t lo, int32_t hi, int *k0, int *k1)
{
    double a, b;

    if (step == 0)
        return start >= lo && start < hi && *k0 < *k1;
    a = ((double)lo - start) / step;
    b = ((double)hi - start) / step;
    if (step < 0) {
        double t = a;
        a = b;
        b = t;
    }
    a = floor(a) - 1;
    b = ceil(b) + 1;
    if (a > *k0)
        *k0 = a < *k1 ? (int)a : *k1;
    if (b < *k1)
        *k1 = b > *k0 ? (int)b : *k0;
    return *k0 < *k1;
}

/* Draws s inside the clip rectangle: the image as the parallelogram centre + u * ex + v * ey
   (u, v in -0.5..0.5), multiplied by its colour, over what is there (straight alpha). The
   texture coordinates step in 16.16 fixed point along each row. */
static void blit(canvas *c, const sprite *s, const int *clip)
{
    float det = s->ex[0] * s->ey[1] - s->ex[1] * s->ey[0];
    int x0 = s->box[0] > clip[0] ? s->box[0] : clip[0], x1 = s->box[2] < clip[2] ? s->box[2] : clip[2];
    int y0 = s->box[1] > clip[1] ? s->box[1] : clip[1], y1 = s->box[3] < clip[3] ? s->box[3] : clip[3];
    /* crop: the part of the image, as x, y, w, h fractions, that covers the whole quad (a
       texture window): The Da Vinci Code scales its cryptex strips to a quarter of their
       height and shows a quarter of each strip's letters in them. */
    float u0 = s->has_crop ? s->crop[0] : 0, v0 = s->has_crop ? s->crop[1] : 0;
    float u1 = s->has_crop ? s->crop[0] + s->crop[2] : 1, v1 = s->has_crop ? s->crop[1] + s->crop[3] : 1;
    int32_t umin, umax, vmin, vmax, dudx, dvdx;
    int cr, cg, cb, ca;
    const uint8_t *occ = s->occ;
    int occ_w = (s->iw + 7) / 8;

    /* A script can compute a colour that is not a number (24: Redemption): nothing to draw. */
    if (isnan(s->color[0]) || isnan(s->color[1]) || isnan(s->color[2]) || !(s->color[3] > 0))
        return;
    cr = (int)(s->color[0] * 256);
    cg = (int)(s->color[1] * 256);
    cb = (int)(s->color[2] * 256);
    ca = (int)(s->color[3] * 256);
    if (fabsf(det) < 0.5f || ca <= 0 || x0 >= x1 || y0 >= y1)
        return;
    if (u0 < 0) u0 = 0;
    if (v0 < 0) v0 = 0;
    if (u1 > 1) u1 = 1;
    if (v1 > 1) v1 = 1;
    /* u, v in image pixels, 16.16 */
    umin = (int32_t)(u0 * (float)s->iw * 65536);
    umax = (int32_t)(u1 * (float)s->iw * 65536);
    vmin = (int32_t)(v0 * (float)s->ih * 65536);
    vmax = (int32_t)(v1 * (float)s->ih * 65536);
    dudx = (int32_t)(s->ey[1] / det * (u1 - u0) * (float)s->iw * 65536);
    dvdx = (int32_t)(-s->ex[1] / det * (v1 - v0) * (float)s->ih * 65536);
    for (int y = y0; y < y1; y++) {
        uint8_t *d = c->rgba + (size_t)y * c->stride + (size_t)x0 * 4;
        float dx = (float)x0 + 0.5f - s->cx, dy = (float)y + 0.5f - s->cy;
        int32_t u = (int32_t)((u0 + ((dx * s->ey[1] - dy * s->ey[0]) / det + 0.5f) * (u1 - u0)) * (float)s->iw * 65536);
        int32_t v = (int32_t)((v0 + ((s->ex[0] * dy - s->ex[1] * dx) / det + 0.5f) * (v1 - v0)) * (float)s->ih * 65536);
        int k0 = 0, k1 = x1 - x0;
        if (!row_span(u, dudx, umin, umax, &k0, &k1) || !row_span(v, dvdx, vmin, vmax, &k0, &k1))
            continue;
        d += (size_t)k0 * 4;
        u = (int32_t)((uint32_t)u + (uint32_t)k0 * (uint32_t)dudx);
        v = (int32_t)((uint32_t)v + (uint32_t)k0 * (uint32_t)dvdx);
        for (int x = x0 + k0; x < x0 + k1; x++, d += 4, u += dudx, v += dvdx) {
            const uint8_t *p;
            int a, da, oa;
            if (u < umin || u >= umax || v < vmin || v >= vmax)
                continue;
            if (occ && !occ[(v >> 19) * occ_w + (u >> 19)])
                continue;             /* an empty 8x8 block: no need to read the image */
            p = s->img + ((size_t)(v >> 16) * s->iw + (size_t)(u >> 16)) * 4;
            a = p[3] * ca >> 8;
            if (a <= 0)
                continue;
            da = d[3];
            if (a >= 255 || da == 0) {
                /* nothing under it to blend with, or fully opaque */
                d[0] = (uint8_t)(p[0] * cr >> 8);
                d[1] = (uint8_t)(p[1] * cg >> 8);
                d[2] = (uint8_t)(p[2] * cb >> 8);
                d[3] = (uint8_t)(a > 255 ? 255 : a);
                continue;
            }
            /* out = src * a + dst * da * (1 - a), over oa = a + da * (1 - a); in 0..255 */
            oa = a + (int)div_small((uint32_t)(da * (255 - a)), 255);
            /* Rounding can reach 256 (white over a white 141-alpha band in Tenjho Tenge), which
               would wrap to black: clamp. */
            for (int k = 0; k < 3; k++) {
                int src = p[k] * (k == 0 ? cr : k == 1 ? cg : cb) >> 8;
                int v = (int)div_small((uint32_t)(src * a) + div_small((uint32_t)(d[k] * da * (255 - a)), 255), (uint32_t)oa);
                d[k] = (uint8_t)(v > 255 ? 255 : v);
            }
            d[3] = (uint8_t)oa;
        }
    }
}

/* Whether an iw x ih image draws at its widget's width x height (STRETCH), at its own size
   (OWN), at its own size because it has the widget's shape but is more than OVER_MAX times
   smaller (OVER), or scaled up evenly to fit inside the widget (FIT). An image stretches to its
   widget's size when the two have the same shape (16:9 cards and backgrounds), the image is a
   fill or a strip at most FILL_MAX across in one direction, four times as long, and spread
   over four times its width or more (a 1x1 black fade; Tenjho Tenge's 10x64 band behind its
   buttons; not Jackass's 55x16 labels), or it is all one colour (12 Monkeys' 28x28 black fade
   over the whole screen) or a gradient in one direction. Artwork made at half or a third of its size fits its widget: Hero
   and Deftones put 53x15 labels in 106x29 widgets, The Texas Chainsaw Massacre 54x14 in 177x49.
   Otherwise it is drawn at its own size: 2Fast2Furious and The Bourne Supremacy size their
   label buttons for the longest translation, and on a PSP each label keeps its own size. */
enum { STRETCH, OWN, OVER, FIT };
#define OVER_MAX 3.3f

static int stretch_rule(int iw, int ih, float width, float height, int fill)
{
    int thin = iw < ih ? iw : ih, thick = iw < ih ? ih : iw;
    float across = iw < ih ? width : height;      /* the widget's size along the image's thin side */
    /* Not a 55x16 label (Jackass), nor Superbad's 119x12 labels in widgets 26 high: a strip
       spreads across 4 times its width or more. */
    int strip = thin <= FILL_MAX && (thick <= FILL_MAX || thick >= 4 * thin) && across >= 4.0f * (float)thin;
    float shape;
    int full, band;

    if (strip || fill || width <= 0 || height <= 0)
        return STRETCH;
    /* A widget one unit wide or high is a unit plane the script sizes by its scale: Full Metal
       Panic's labels are 162x1 with a y scale of 30, their image's height. Judged by the
       widget's size alone they looked 160 times too tall for their shape and kept the image's
       size, which the scale then multiplied by 30. */
    if (width <= 2.0f || height <= 2.0f)
        return STRETCH;
    shape = (width / height) / ((float)iw / (float)ih);
    /* cards and backgrounds match within 3% (Austin Powers' 427x240 mask in 854x490).
       A full-screen background stretches whatever its size (The Matrix's 214x120 scene
       background); anything smaller at most 3 times: half-size artwork doubles, while The
       Goonies' subtitle labels, 4 times over, keep their size */
    /* Full-width artwork (half or full size across the screen) stretches whatever its height:
       Stargate's transition bands put the 427x27 title band in 854x85 widgets. 4:3 warning
       cards in full-screen widgets keep their shape. */
    full = width >= 0.9f * UNITS_W;
    band = full && (fabsf(2.0f * (float)iw - width) <= 0.03f * width || fabsf((float)iw - width) <= 0.03f * width);
    if (band)
        return STRETCH;
    /* A widget larger than the screen is artwork that sweeps across or covers it, never a
       label or a card: The Goonies' 256x48 blur fills its 1700x480 widget as it slides between
       menus, as on a PSP (Batman Begins' gradient, The Fifth Element's black line too). */
    if (width > 1.05f * UNITS_W || height > 1.05f * UNITS_H)
        return STRETCH;
    if (shape > 1.03f || shape < 0.97f) {
        float rw = width / (float)iw, rh = height / (float)ih, lo = rw < rh ? rw : rh, hi = rw < rh ? rh : rw;
        /* A label a little wider than its widget squeezes into it: Eight Below's 161x28 "Sneak
           Peeks" in a 121x28 button sits between the chevrons placed for it. */
        if (hi <= 1.0f && lo >= 0.6f)
            return STRETCH;
        /* Half-size pictures that nearly fit their widget fill it: Eight Below's 279x227 dog in
           a 548x500 plane has its feet on the screen's bottom edge, which fitting left 13
           units above it. Labels and cards keep their shape. */
        if (lo >= 1.5f && lo <= OVER_MAX && hi <= 1.15f * lo && width >= 200.0f && height >= 200.0f)
            return STRETCH;
        return lo >= 1.5f && lo <= OVER_MAX && hi <= 1.6f * lo ? FIT : OWN;
    }
    return width > OVER_MAX * (float)iw && !full ? OVER : STRETCH;
}

/* Buttons side by side at one scale show their labels at one size: when one of them keeps its
   image's size (over OVER_MAX times smaller than its widget), the others under the same parent with
   the same scale do too. The Goonies' language page has its spoken languages in widgets twice
   their images and its subtitles in widgets four times theirs, both at scale 1.8 and drawn in
   one font; stretching the first drew them twice the size of the second. */
/* 1 if a button draws the image at its own size: one that keeps it (own_size, OWN, OVER). */
static int drawn_own_size(const res *r, int image)
{
    const rco *rc = &r->r;
    int iw, ih;

    if (image < 0 || png_info(rc->images[image].data, rc->images[image].size, &iw, &ih) < 0)
        return 0;
    for (int j = 0; j < rc->object_count; j++) {
        const rco_object *b = &rc->objects[j];
        int rule;
        if (b->kind != RCO_BUTTON || b->image != image)
            continue;
        rule = r->own_size[j] ? OWN : stretch_rule(iw, ih, b->size[0], b->size[1], 0);
        if (rule == OWN || rule == OVER)
            return 1;
    }
    return 0;
}

static void same_size_labels(menu *m, res *r)
{
    const rco *rc = &r->r;

    for (int i = 0; i < rc->object_count; i++) {
        const rco_object *o = &rc->objects[i];
        int iw, ih, rule;
        if (o->kind != RCO_BUTTON || o->image < 0 || r->own_size[i] ||
            png_info(rc->images[o->image].data, rc->images[o->image].size, &iw, &ih) < 0)
            continue;
        /* A label at its own size in a widget three times its width or more (Dragon Wars' audio
           page: English 52x21 in 246x30) sets the size for its row too: French, 48x15 in a 111x34
           widget of nearly its shape, stretched to 2.3 times the size of English. */
        rule = stretch_rule(iw, ih, o->size[0], o->size[1], 0);
        if (rule != OVER && !(rule == OWN && o->size[0] >= 3.0f * (float)iw))
            continue;
        for (int j = 0; j < rc->object_count; j++) {
            const rco_object *b = &rc->objects[j];
            int bw, bh;
            if (j == i || r->own_size[j] || b->kind != RCO_BUTTON || b->parent != o->parent || b->scale[0] != o->scale[0] ||
                b->image < 0 || png_info(rc->images[b->image].data, rc->images[b->image].size, &bw, &bh) < 0)
                continue;
            int rule = stretch_rule(bw, bh, b->size[0], b->size[1], 0);
            if ((rule == STRETCH || rule == FIT) && b->size[0] > 1.03f * (float)bw) {
                r->own_size[j] = 1;
                mlog(m, "menu: %s keeps its image size, as %s does", b->name ? b->name : "?", o->name ? o->name : "?");
            }
        }
    }
    /* Labels beside a label scaled up to fit its widget (FIT) all take the smallest scale of
       the row, one at its own size counting as 1 and a stretched one as its smaller stretch:
       Superbad's subtitle languages, 64x16 to 194x16 images in widgets of 105x26 to 214x27,
       draw at their own size, as its audio page draws the same English image, and its trailer
       "Gabriel" (64x13 in 182x32) matches "Walk Hard" (184x31 in 195x32). Shrinking the row to
       fit Portuguese (194) in its 123 widget drew English at 0.63 here and 1 on the audio
       page. An image another button draws at its own size counts as 1 too: the other Superbad
       (eng, fra, spa) has 73x18 to 110x12 subtitle images that all fit their widgets at 1.52,
       while its audio page draws the same English image at its own size. */
    for (int i = 0; i < rc->object_count; i++) {
        const rco_object *o = &rc->objects[i];
        int iw, ih;
        float k, lo;
        if (o->kind != RCO_BUTTON || o->image < 0 || r->own_size[i] ||
            png_info(rc->images[o->image].data, rc->images[o->image].size, &iw, &ih) < 0 ||
            stretch_rule(iw, ih, o->size[0], o->size[1], 0) != FIT)
            continue;
        k = o->size[0] / (float)iw < o->size[1] / (float)ih ? o->size[0] / (float)iw : o->size[1] / (float)ih;
        lo = drawn_own_size(r, o->image) ? 1 : k;
        for (int j = 0; j < rc->object_count; j++) {
            const rco_object *b = &rc->objects[j];
            int bw, bh, rule;
            if (j == i || b->kind != RCO_BUTTON || b->parent != o->parent || b->scale[0] != o->scale[0] ||
                b->image < 0 || png_info(rc->images[b->image].data, rc->images[b->image].size, &bw, &bh) < 0)
                continue;
            rule = r->own_size[j] ? OWN : stretch_rule(bw, bh, b->size[0], b->size[1], 0);
            float kb = b->size[0] / (float)bw < b->size[1] / (float)bh ? b->size[0] / (float)bw : b->size[1] / (float)bh;
            if (rule == OWN || rule == OVER || drawn_own_size(r, b->image))    /* at its own size, even wider than its widget */
                kb = 1;
            if (kb > 0)
                lo = lo < kb ? lo : kb;
        }
        if (lo >= k)
            continue;
        for (int j = 0; j < rc->object_count; j++) {    /* the whole row, this label included */
            const rco_object *b = &rc->objects[j];
            if (b->kind == RCO_BUTTON && b->parent == o->parent && b->scale[0] == o->scale[0] && b->image >= 0 &&
                (r->label_k[j] == 0 || r->label_k[j] > lo)) {
                r->label_k[j] = lo;
                mlog(m, "menu: %s scales its image by %.2f, as the labels beside it", b->name ? b->name : "?", lo);
            }
        }
    }
}

static void collect(res *r, canvas *c, int i, const xform *parent, sprite *list, int *count)
{
    const wstate *w = &r->w[i];
    float lm[3][3];
    xform x;
    float own[4];

    /* A widget's colour and alpha are its own picture's: they do not reach its children, which
       the scripts fade one by one (Species has fadeChildrenAlpha). Species and Terminator keep
       their whole menu wheel under Wheel, a picture at alpha 0, and Little Britain its menu
       cube under plnMMFloat, an empty plane at alpha 0. A fade (an animation's fade step, on
       pages and groups) does reach them. */
    for (int k = 0; k < 4; k++) {
        own[k] = parent->color[k] * w->color[k];
        x.color[k] = parent->color[k];
    }
    own[3] *= w->fade;
    x.color[3] *= w->fade;
    if (x.color[3] <= 0.004f || w->hidden)
        return;
    /* Position in the parent's frame, then this widget's own rotation and scale. */
    for (int k = 0; k < 3; k++)
        x.t[k] = parent->t[k] + parent->l[k][0] * w->pos[0] + parent->l[k][1] * w->pos[1] + parent->l[k][2] * w->pos[2];
    local_matrix(lm, w->view_rot_on ? w->view_rot : w->rot, w->scale);
    mat_mul(x.l, (float (*)[3])parent->l, lm);
    /* A plane with a size and no image is a rectangle of its colour: Underworld Evolution's
       plnBlackBack, 858x36 in black, is the band behind its menu labels, and masks such as
       Kill Bill Vol. 2's plnSSMask cover the screen when the script colours them. */
    int image = w->image >= 0 ? w->image
              : r->r.objects[i].kind == RCO_PLANE && w->size[0] > 0 && w->size[1] > 0 ? r->r.image_count : -1;
    if (image >= 0 && r->r.objects[i].kind != RCO_PAGE && r->r.objects[i].kind != RCO_GROUP && *count < MAX_SPRITES) {
        int iw, ih;
        const uint8_t *img = image_rgba(r, image, &iw, &ih);
        if (img) {
            sprite *s = &list[*count];
            float sxu = (float)c->w / UNITS_W, syu = (float)c->h / UNITS_H;
            float width = w->size[0] > 0 ? w->size[0] : (float)iw, height = w->size[1] > 0 ? w->size[1] : (float)ih;
            int rule = r->own_size[i] ? OWN : stretch_rule(iw, ih, width, height, r->img_fill[image]);
            /* A frame over the whole screen, with clear parts, covers it whatever its shape:
               Kill Bill Vol. 2 frames its scene previews with a 252x168 picture in an 854x480
               widget, and fitted to the height it left the preview showing at the sides.
               Opaque cards (Clerks' 512x341 rating card) keep their shape. */
            if (rule == FIT && width >= 0.9f * UNITS_W && height >= 0.9f * UNITS_H && r->img_clear[image])
                rule = STRETCH;
            /* A cropped part of a strip larger than its widget fills the widget: Enter the Dragon
               shows one scene number of its 59x512 strip, squashed numerals 18 high, in a 59x55
               widget. Kept at the strip's size, the number stretched down the screen as a column.
               Ultraviolet's 304x275 panel in a 450x275 widget keeps its size as it unfolds. */
            if (w->has_crop && w->size[0] > 0 && w->size[1] > 0 && ((float)iw > width || (float)ih > height))
                rule = STRETCH;
            /* A title fits inside the box it sits in: Dragon Wars' 123x16 "Subtitles" in a 214x27
               widget, scaled up evenly, was 208 wide on a 184x149 parchment; it keeps its size,
               as the audio page's title does. */
            {
                int pa = r->r.objects[i].parent, pw, ph;
                int pimg = pa >= 0 ? r->w[pa].image : -1;
                float k = width / (float)iw < height / (float)ih ? width / (float)iw : height / (float)ih;
                float drawn = rule == FIT ? (float)iw * k : width;
                if ((rule == FIT || rule == STRETCH) && r->label_k[i] == 0 && pimg >= 0 && r->r.objects[pa].kind == RCO_PLANE &&
                    r->w[pa].size[0] <= 0 && png_info(r->r.images[pimg].data, r->r.images[pimg].size, &pw, &ph) == 0 &&
                    iw <= pw && drawn > 1.03f * (float)iw && drawn > (float)pw)
                    rule = OWN;
            }
            /* A plane the script gives pictures of different sizes shows them all at one
               magnification, their own: High Tension's scene titles, 55x17 to 163x14 and one
               127x40 of two lines, go in turn into one 326x80 plane, the box around them all
               at twice their size. Judged one by one, the short ones stretched to fill it as
               strips, others doubled and some kept their size. At their own size the two-line
               title ends just under the SCENE INDEX heading. Pictures of the plane's shape
               (a highlight swapped for another) still fill it. */
            if (w->script_image && r->r.objects[i].kind == RCO_PLANE && !r->img_fill[image] &&
                (rule == FIT || rule == STRETCH) && width < 0.9f * UNITS_W && height < 0.9f * UNITS_H &&
                fabsf((width / height) / ((float)iw / (float)ih) - 1.0f) > 0.03f)
                rule = OWN;
            if (r->label_k[i] > 0) {
                width = (float)iw * r->label_k[i];
                height = (float)ih * r->label_k[i];
            } else if (rule == FIT) {
                float k = width / (float)iw < height / (float)ih ? width / (float)iw : height / (float)ih;
                width = (float)iw * k;
                height = (float)ih * k;
            } else if (rule != STRETCH) {
                width = (float)iw;
                height = (float)ih;
            }
            /* Anchored images: pos is an edge or a corner (Toy Story places its backgrounds by
               their top-left corners), so move the centre half a width or height from it. */
            int ax = r->r.objects[i].anchor & 0xF, ay = (r->r.objects[i].anchor >> 4) & 0xF;
            float shift = ax == 1 ? width / 2 : ax == 2 ? -width / 2 : 0;
            float vshift = ay == 1 ? -height / 2 : ay == 2 ? height / 2 : 0;   /* y goes up */
            memset(s, 0, sizeof(*s));
            s->widget = i;
            s->img = img;
            s->occ = r->occ[image];
            s->image = image;
            s->iw = iw;
            s->ih = ih;
            /* Image axes on screen: x right, and y down (the image's rows go down). */
            /* Images are flat sprites facing the viewer: turns about X and Y move them in
               3D (Batman's carousel, the wheels of Christmas With the Kranks) but do not tilt
               them; the turn in the screen's plane and each axis's scale remain. */
            /* A negative scale mirrors the image (Little Britain's left arrow): kept. */
            float det = x.l[0][0] * (x.l[1][1] * x.l[2][2] - x.l[1][2] * x.l[2][1]) -
                        x.l[0][1] * (x.l[1][0] * x.l[2][2] - x.l[1][2] * x.l[2][0]) +
                        x.l[0][2] * (x.l[1][0] * x.l[2][1] - x.l[1][1] * x.l[2][0]);
            float mx = det < 0 ? -1.0f : 1.0f;
            /* A widget left facing straight away, by turns of its own and of the groups above,
               shows its picture from behind, mirrored: Underworld Evolution draws the right
               halves of its ornaments by turning left halves by pi about Y, and the lower halves
               by pi about X (a mirror then reads as upside down). To Die For turns its labels by
               pi inside a drum turned by -pi: they face the viewer. Other turns, and turns on
               the way, stay as described above: The Texas Chainsaw Massacre turns its labels by
               up to 2.5 about Y as they slide. */
            float zl = sqrtf(x.l[0][2] * x.l[0][2] + x.l[1][2] * x.l[1][2] + x.l[2][2] * x.l[2][2]);
            if (!w->view_rot_on && zl > 0 && x.l[2][2] < -0.999f * zl)
                mx = -mx;
            /* The angle in the screen's plane from the image's up axis as projected (its x axis
               when the up axis is end-on): Batman tilts its carousel back by 0.1, and the two
               axes together read that as a turn of 6 degrees, or 180 at the back. */
            float ang = fabsf(x.l[0][1]) + fabsf(x.l[1][1]) > 0.001f ? atan2f(-x.l[0][1], x.l[1][1])
                                                                     : atan2f(mx * x.l[1][0], mx * x.l[0][0]);
            float lx = sqrtf(x.l[0][0] * x.l[0][0] + x.l[1][0] * x.l[1][0] + x.l[2][0] * x.l[2][0]);
            float ly = sqrtf(x.l[0][1] * x.l[0][1] + x.l[1][1] * x.l[1][1] + x.l[2][1] * x.l[2][1]);
            float ca = cosf(ang), sa = sinf(ang);
            /* Perspective: z comes toward the viewer, a camera PERSP_D units from the screen
               plane. The Boondock Saints and Cowboy Bebop lay their menus out at half size
               (427x240 backgrounds) and bring them to z 253.834, where they double to fill
               the screen; Foo Fighters' 480x272 backgrounds at z 220 grow by 1.77 to fill it.
               Feel the Noise sets its menu drum back at z -250. Widgets at z 0, most of them,
               keep their size. */
            float pz = x.t[2] > PERSP_D - 1 ? PERSP_D - 1 : x.t[2], f = PERSP_D / (PERSP_D - pz);
            float o00 = mx * ca * lx * f, o10 = mx * sa * lx * f, o01 = -sa * ly * f, o11 = ca * ly * f;
            s->ex[0] = o00 * width * sxu;
            s->ex[1] = -o10 * width * syu;
            s->ey[0] = -o01 * height * sxu;
            s->ey[1] = o11 * height * syu;
            s->cx = (UNITS_W / 2 + x.t[0] * f + o00 * shift + o01 * vshift) * sxu;
            s->cy = (UNITS_H / 2 - x.t[1] * f - o10 * shift - o11 * vshift) * syu;
            memcpy(s->color, own, sizeof(s->color));
            s->has_crop = w->has_crop;
            if (w->has_crop)
                memcpy(s->crop, w->crop, sizeof(s->crop));
            sprite_box(s, c->w, c->h);
            /* A button tinted black whose picture is a filled black box is not drawn: on a
               PSP, Starship Troopers (eng) shows its chosen scene by the name's colour, with no
               sign of the 448x95 box its scene button (tinted 0, 0, 0) holds, while the boxes
               of its language buttons (tinted white) mark the chosen language. The box stays
               in the list, clear, for touch. */
            if (r->r.objects[i].kind == RCO_BUTTON && r->img_plain[image] &&
                r->r.objects[i].color[0] == 0 && r->r.objects[i].color[1] == 0 && r->r.objects[i].color[2] == 0)
                s->color[3] = 0;
            if (s->box[2] > s->box[0] && s->box[3] > s->box[1])
                (*count)++;
        }
    }
    for (int ch = r->r.objects[i].first_child; ch >= 0; ch = r->r.objects[ch].next_sibling)
        collect(r, c, ch, &x, list, count);
}

static void grow_box(int *b, const int *add)
{
    if (add[2] <= add[0] || add[3] <= add[1])
        return;
    if (b[2] <= b[0] || b[3] <= b[1]) {
        memcpy(b, add, 4 * sizeof(int));
        return;
    }
    if (add[0] < b[0]) b[0] = add[0];
    if (add[1] < b[1]) b[1] = add[1];
    if (add[2] > b[2]) b[2] = add[2];
    if (add[3] > b[3]) b[3] = add[3];
}

static int same_sprite(const sprite *a, const sprite *b)
{
    return a->widget == b->widget && a->img == b->img && memcmp(a->box, b->box, sizeof(a->box)) == 0 &&
           fabsf(a->cx - b->cx) < 0.01f && fabsf(a->cy - b->cy) < 0.01f && fabsf(a->ex[0] - b->ex[0]) < 0.01f &&
           fabsf(a->ex[1] - b->ex[1]) < 0.01f && fabsf(a->ey[0] - b->ey[0]) < 0.01f &&
           fabsf(a->ey[1] - b->ey[1]) < 0.01f && memcmp(a->color, b->color, sizeof(a->color)) == 0 &&
           a->has_crop == b->has_crop && (!a->has_crop || memcmp(a->crop, b->crop, sizeof(a->crop)) == 0);
}

void menu_draw(menu *m, canvas *c)
{
    static sprite lists[2][MAX_SPRITES];
    static int counts[2], cur, valid;
    res *r = m->res;
    sprite *now = lists[cur ^ 1], *before = lists[cur];
    int n = 0, dirty[4] = {0, 0, 0, 0};
    xform root;

    if (!recip[2])
        init_recip();

    memset(&root, 0, sizeof(root));
    for (int k = 0; k < 3; k++)
        root.l[k][k] = 1;
    for (int k = 0; k < 4; k++)
        root.color[k] = 1;
    /* Pages draw in their order in the RCO, not the order they opened: a script that reopens
       a background page must not cover the pages in front of it. */
    for (int i = 0; i < r->r.object_count; i++)
        if (r->r.objects[i].kind == RCO_PAGE)
            for (int p = 0; p < r->page_count; p++)
                if (r->pages[p] == i)
                    collect(r, c, i, &root, now, &n);
    /* The region to redraw: every sprite that differs from the last picture, where it was
       and where it is now. A new resource or canvas starts from a full redraw. */
    if (!valid || !m->drawn_once) {
        dirty[2] = c->w;
        dirty[3] = c->h;
        m->drawn_once = 1;
    } else {
        int k = 0;
        for (; k < n && k < counts[cur] && same_sprite(&now[k], &before[k]); k++)
            ;
        for (int j = k; j < n; j++)
            grow_box(dirty, now[j].box);
        for (int j = k; j < counts[cur]; j++)
            grow_box(dirty, before[j].box);
    }
    if (dirty[2] > dirty[0] && dirty[3] > dirty[1]) {
        canvas_rect(c, dirty[0], dirty[1], dirty[2] - dirty[0], dirty[3] - dirty[1], 0);
        for (int k = 0; k < n; k++)
            blit(c, &now[k], dirty);
    }
    counts[cur ^ 1] = n;
    cur ^= 1;
    valid = 1;
    m->dirty = 0;
}

int menu_sprites(menu *m, plat_sprite *out, int max, int *shown)
{
    static sprite list[MAX_SPRITES];
    canvas c = {NULL, 960, 544, 960 * 4};
    res *r = m->res;
    int n = 0, count = 0;
    xform root;

    memset(&root, 0, sizeof(root));
    for (int k = 0; k < 3; k++)
        root.l[k][k] = 1;
    for (int k = 0; k < 4; k++)
        root.color[k] = 1;
    /* as menu_draw: pages in their order in the RCO */
    for (int i = 0; i < r->r.object_count; i++)
        if (r->r.objects[i].kind == RCO_PAGE)
            for (int p = 0; p < r->page_count; p++)
                if (r->pages[p] == i)
                    collect(r, &c, i, &root, list, &n);
    *shown = 0;
    for (int k = 0; k < n && count < max; k++) {
        const sprite *s = &list[k];
        plat_sprite *o = &out[count];
        float det = s->ex[0] * s->ey[1] - s->ex[1] * s->ey[0];
        float u0 = s->has_crop ? s->crop[0] : 0, v0 = s->has_crop ? s->crop[1] : 0;
        float u1 = s->has_crop ? s->crop[0] + s->crop[2] : 1, v1 = s->has_crop ? s->crop[1] + s->crop[3] : 1;
        int any = 0;
        /* what blit leaves out */
        if (isnan(s->color[0]) || isnan(s->color[1]) || isnan(s->color[2]) || !(s->color[3] * 256 >= 1) ||
            fabsf(det) < 0.5f || s->box[0] >= s->box[2] || s->box[1] >= s->box[3])
            continue;
        if (u0 < 0) u0 = 0;
        if (v0 < 0) v0 = 0;
        if (u1 > 1) u1 = 1;
        if (v1 > 1) v1 = 1;
        if (u0 >= u1 || v0 >= v1)
            continue;
        o->rgba = s->img;
        o->w = s->iw;
        o->h = s->ih;
        o->id = r->img_id[s->image];
        /* the quad is centre +- ex / 2 +- ey / 2, and the crop (u0..u1, v0..v1) covers it all */
        for (int j = 0; j < 4; j++) {
            float u = j & 1 ? 0.5f : -0.5f, v = j & 2 ? 0.5f : -0.5f;
            o->x[j] = s->cx + u * s->ex[0] + v * s->ey[0];
            o->y[j] = s->cy + u * s->ex[1] + v * s->ey[1];
        }
        o->u0 = u0;
        o->v0 = v0;
        o->u1 = u1;
        o->v1 = v1;
        memcpy(o->color, s->color, sizeof(o->color));
        if (o->color[3] > 1)
            o->color[3] = 1;
        for (int b = 0; s->occ && !any && b < ((s->iw + 7) / 8) * ((s->ih + 7) / 8); b++)
            any = s->occ[b];
        *shown |= any || !s->occ;
        count++;
    }
    m->dirty = 0;
    return count;
}

/* ---- touch ---- */

#define SWIPE_MIN 60          /* screen pixels a finger moves for a swipe */
#define TAP_MAX   24          /* and at most, for a tap */
#define TAP_NEAR  100         /* a tap this close to a button, and on no other, chooses it */

/* The button on the active page under (x, y) on the 960x544 screen, or -1: the topmost visible
   button image there, else the nearest within TAP_NEAR. Bourne's scene buttons are the numbers
   under the thumbnails, so a tap on a thumbnail picks the number below it. */
static int button_at(menu *m, int x, int y)
{
    static sprite list[MAX_SPRITES];
    res *r = m->res;
    canvas c = {NULL, 960, 544, 960 * 4};
    xform root;
    int n = 0;

    memset(&root, 0, sizeof(root));
    for (int k = 0; k < 3; k++)
        root.l[k][k] = 1;
    for (int k = 0; k < 4; k++)
        root.color[k] = 1;
    if (input_page(r) < 0)
        return -1;
    collect(r, &c, input_page(r), &root, list, &n);
    for (int k = n - 1; k >= 0; k--) {
        const sprite *s = &list[k];
        if (r->r.objects[s->widget].kind == RCO_BUTTON && s->color[3] > 0.05f &&
            x >= s->box[0] && x < s->box[2] && y >= s->box[1] && y < s->box[3])
            return s->widget;
    }
    {
        int best = -1, best_d = TAP_NEAR * TAP_NEAR + 1;
        for (int k = 0; k < n; k++) {
            const sprite *s = &list[k];
            int ddx = x < s->box[0] ? s->box[0] - x : x >= s->box[2] ? x - s->box[2] + 1 : 0;
            int ddy = y < s->box[1] ? s->box[1] - y : y >= s->box[3] ? y - s->box[3] + 1 : 0;
            if (r->r.objects[s->widget].kind == RCO_BUTTON && s->color[3] > 0.05f && ddx * ddx + ddy * ddy < best_d) {
                best = s->widget;
                best_d = ddx * ddx + ddy * ddy;
            }
        }
        return best;
    }
}

void menu_touch(menu *m, int down, int x, int y)
{
    res *r = m->res;
    int dx, dy, b;

    if (down) {
        if (!m->touch_down) {
            m->touch_down = 1;
            m->touch_x0 = x;
            m->touch_y0 = y;
        }
        m->touch_x = x;
        m->touch_y = y;
        return;
    }
    if (!m->touch_down)
        return;
    m->touch_down = 0;
    if (input_page(r) < 0 || m->lock)
        return;
    dx = m->touch_x - m->touch_x0;
    dy = m->touch_y - m->touch_y0;
    /* A swipe is a direction button, so menus that move their highlight in script follow it. */
    if (abs(dx) >= SWIPE_MIN || abs(dy) >= SWIPE_MIN) {
        if (abs(dx) > abs(dy))
            do_input(m, dx > 0 ? PLAT_BTN_RIGHT : PLAT_BTN_LEFT);
        else
            do_input(m, dy > 0 ? PLAT_BTN_DOWN : PLAT_BTN_UP);
        return;
    }
    if (abs(dx) > TAP_MAX || abs(dy) > TAP_MAX)
        return;
    if ((b = button_at(m, m->touch_x0, m->touch_y0)) < 0) {
        /* Nothing to tap: menus that move a highlight in script (Summer Days' choices, a Nav
           button) get Cross, so a swipe picks and a tap chooses. */
        do_input(m, PLAT_BTN_CROSS);
        return;
    }
    /* A tap on a button: focus it, as the D-pad would, then choose it. */
    if (r->focus[input_page(r)] != b) {
        set_focus(m, b);
        if (m->res != r || m->lock)
            return;
    }
    do_action_named(m, &r->r.objects[b].action[RCO_BTN_PUSH], b, "onPush");
}
