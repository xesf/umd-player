/* menutest: checks the UMD Video menu code on the host.
   menutest <file.rco>                print what src/core/rco.c reads
   menutest -r <file.rco>             run the menu script with a player that only logs: prints
                                      every call and property the script uses, then starts the
                                      disc (controller.onAutoPlay)
   menutest -d <iso> <seconds> [<time>:<button>]... [-o <dir>]
                                      run the disc's menus (src/app/menu.c) with a simulated
                                      player on a virtual clock: titles end after their length,
                                      buttons are pressed at the given times (up down left right
                                      cross circle triangle start, menu for the menu button
                                      during playback, hold:<button>:<seconds>, tap:x:y and swipe:x0:y0:x1:y1 on the
                                      960x544 touch screen), and with -o every new menu
                                      picture is saved as dir/menu-<ms>.ppm, over the 960x544
                                      PNG given with -b (a frame of the menu video). With -s,
                                      the disc continues from the state in <file> if there is
                                      one (controller.onContinuePlay), and leaves at the end
                                      (controller.onExit) saving its state there, as
                                      src/app/disc.c does */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app/menu.h"
#include "../src/core/canvas.h"
#include "../src/core/iso9660.h"
#include "../src/core/png.h"
#include "../src/core/rco.h"
#include "../src/core/umdv.h"
#include "../src/core/vsmx.h"
#include "../src/platform/platform.h"
#include "host_source.h"

static uint8_t *load(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

static const char *nm(const char *s) { return s ? s : "-"; }

/* ---- a host that logs: every property is another probe, every call is printed ---- */

typedef struct probe {
    char path[160];
} probe;

static int probe_get(vsmx *vm, void *host, const char *key, vs_value *out);
static int probe_set(vsmx *vm, void *host, const char *key, vs_value value);
static void probe_call(vsmx *vm, void *host, vs_value self, const vs_value *args, int argc, vs_value *ret);
static const vs_class probe_class = {"probe", probe_get, probe_set, probe_call, NULL};

static vs_value new_probe(vsmx *vm, const char *path)
{
    probe *p = calloc(1, sizeof(probe));     /* kept for the run; this is a test tool */
    snprintf(p->path, sizeof(p->path), "%s", path);
    return vs_new_host(vm, &probe_class, p);
}

static int probe_get(vsmx *vm, void *host, const char *key, vs_value *out)
{
    char path[200];
    snprintf(path, sizeof(path), "%s.%s", ((probe *)host)->path, key);
    *out = new_probe(vm, path);
    return 1;
}

static void show(vs_value v, char *out, int size)
{
    char buf[64];
    if (v.type == VS_STR)
        snprintf(out, (size_t)size, "\"%s\"", vs_text(v, buf, sizeof(buf)));
    else if (vs_is_function(v))
        snprintf(out, (size_t)size, "function");
    else if (v.type == VS_OBJ)
        snprintf(out, (size_t)size, vs_length(v) ? "[array %d]" : "object", vs_length(v));
    else
        snprintf(out, (size_t)size, "%s", vs_text(v, buf, sizeof(buf)));
}

static int probe_set(vsmx *vm, void *host, const char *key, vs_value value)
{
    char v[80];
    (void)vm;
    show(value, v, sizeof(v));
    printf("  set %s.%s = %s\n", ((probe *)host)->path, key, v);
    return 0;                 /* the object keeps it, so the script can read it back */
}

static void probe_call(vsmx *vm, void *host, vs_value self, const vs_value *args, int argc, vs_value *ret)
{
    char line[512] = "", v[80];
    (void)vm;
    (void)self;
    for (int i = 0; i < argc; i++) {
        show(args[i], v, sizeof(v));
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", i ? ", " : "", v);
    }
    printf("  call %s(%s)\n", ((probe *)host)->path, line);
    *ret = vs_undef();
}

static void log_line(void *user, const char *msg)
{
    (void)user;
    printf("  ! %s\n", msg);
}

static int run_script(const rco *r)
{
    static const char *const hosts[] = {"movieplayer", "controller", "resource", "timer", "sound"};
    vsmx *vm = vs_create(r->script, r->script_size, log_line, NULL);
    vs_value controller;

    if (!vm) {
        printf("cannot load the script\n");
        return 1;
    }
    for (unsigned i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
        vs_set_global(vm, hosts[i], new_probe(vm, hosts[i]));
    printf("-- top level\n");
    if (vs_run_main(vm) < 0)
        printf("top level failed\n");
    controller = vs_global(vm, "controller");
    printf("-- controller.onAutoPlay\n");
    if (vs_call(vm, vs_get(vm, controller, vs_intern(vm, "onAutoPlay")), controller, NULL, 0, NULL) < 0)
        printf("no onAutoPlay handler\n");
    vs_destroy(vm);
    return 0;
}

/* ---- disc mode: menu.c with a simulated player ---- */

typedef struct sim {
    iso_volume vol;
    umdv_playlist pl;
    menu_status st;
    uint64_t now, title_end, title_start, seek_until;  /* virtual time, us */
    int start_chapter;
    int ready_page;           /* the active page with buttons last reported as READY, or -1 */
    uint64_t chapter_at[64];
    int chapter_count;
    int ended_pending;
    int saved_playlist, saved_chapter;
    int last_chapter;         /* the chapter last reported; 0 after a play, as in src/app/disc.c */
    uint64_t events_done;     /* event marks before this position (us) have been reported */
    int menu_mode;            /* the title playing is a menu video */
    const menu *m;            /* once open: which titles keep a place to resume */
} sim;

static uint8_t *sim_read(void *user, const char *path, uint32_t *size)
{
    sim *s = user;
    iso_entry e;
    uint8_t *buf;

    if (iso_find(&s->vol, path, &e) < 0 || !(buf = malloc(e.size ? e.size : 1)))
        return NULL;
    if (iso_read(&s->vol, &e, 0, buf, e.size) != (int64_t)e.size) {
        free(buf);
        return NULL;
    }
    *size = e.size;
    return buf;
}

static void sim_play(void *user, int playlist, int chapter, int audio, int subtitle, int menu_mode)
{
    sim *s = user;
    const umdv_title *t;
    uint64_t len, from = 0;

    printf("%8.2f s  PLAY title %d chapter %d audio %d subtitle %d", s->now / 1e6, playlist, chapter, audio, subtitle);
    if (playlist < 1 || playlist > s->pl.title_count) {
        printf("  (no such title)\n");
        return;
    }
    if (s->st.playing && !s->menu_mode && s->st.playlist != playlist) {
        s->saved_playlist = s->st.playlist;   /* as src/app/disc.c: the replaced title's place */
        s->saved_chapter = s->st.chapter;
    }
    t = &s->pl.titles[playlist - 1];
    len = (uint64_t)(t->end_pts - t->start_pts) * 100 / 9;
    if (chapter > 1 && chapter <= t->chapter_count)
        from = (uint64_t)(s->pl.chapters[t->first_chapter + chapter - 1].pts - t->start_pts) * 100 / 9;
    printf("  \"%s\", %.1f s\n", t->name, (len - from) / 1e6);
    {
        /* as src/app/disc.c: a new title or a jump over 10 s waits 1 s for the drive's seek */
        uint64_t seek = 1000000;
        if (s->st.playing && s->st.playlist == playlist) {
            uint64_t cur = s->title_end > s->now ? len - (s->title_end - s->now) : len;
            if ((cur > from ? cur - from : from - cur) <= 10000000)
                seek = 0;
        }
        s->seek_until = s->now + seek;
    }
    s->st.playing = 1;
    s->st.paused = 0;
    s->st.playlist = playlist;
    s->st.chapter = chapter;
    s->st.audio = audio;
    s->st.subtitle = subtitle;
    s->title_end = s->now + (len > from ? len - from : 0);
    s->title_start = s->now;
    s->start_chapter = chapter > 1 && chapter <= t->chapter_count ? chapter : 1;
    s->last_chapter = 0;
    s->events_done = from;
    s->menu_mode = menu_mode;
}

static void sim_stop(void *user)
{
    sim *s = user;
    printf("%8.2f s  STOP\n", s->now / 1e6);
    /* As src/app/disc.c: only a main video keeps a place; menu videos, short clips and a stop
       with nothing playing keep the last. Die Hard stops its logo and resumes, expecting
       resume() to fail so that it plays the movie. */
    if (s->st.playing && !s->menu_mode && (!s->m || menu_title_resumable(s->m, s->st.playlist))) {
        s->saved_playlist = s->st.playlist;
        s->saved_chapter = s->st.chapter;
    }
    s->st.playing = 0;
    s->st.playlist = 0;
}

static int sim_resume(void *user)
{
    sim *s = user;
    printf("%8.2f s  RESUME\n", s->now / 1e6);
    if (!s->saved_playlist)
        return -1;
    sim_play(user, s->saved_playlist, s->saved_chapter, s->st.audio, s->st.subtitle, 0);
    return 0;
}

static void sim_status(void *user, menu_status *st)
{
    *st = ((sim *)user)->st;
}

static void sim_audio(void *user, int audio)
{
    sim *s = user;
    printf("%8.2f s  AUDIO %d\n", s->now / 1e6, audio);
    s->st.audio = audio;
}

static void sim_subtitle(void *user, int subtitle)
{
    sim *s = user;
    printf("%8.2f s  SUBTITLE %d\n", s->now / 1e6, subtitle);
    s->st.subtitle = subtitle;
}

static void sim_window(void *user, float x, float y, float w, float h)
{
    printf("%8.2f s  WINDOW %g,%g %gx%g\n", ((sim *)user)->now / 1e6, x, y, w, h);
}

static void sim_log(void *user, const char *msg)
{
    printf("%8.2f s  %s\n", ((sim *)user)->now / 1e6, msg);
}

static uint32_t button(const char *name)
{
    static const struct { const char *n; uint32_t b; } map[] = {
        {"up", PLAT_BTN_UP}, {"down", PLAT_BTN_DOWN}, {"left", PLAT_BTN_LEFT}, {"right", PLAT_BTN_RIGHT},
        {"cross", PLAT_BTN_CROSS}, {"circle", PLAT_BTN_CIRCLE}, {"triangle", PLAT_BTN_TRIANGLE},
        {"start", PLAT_BTN_START}, {"square", PLAT_BTN_SQUARE}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(name, map[i].n) == 0)
            return map[i].b;
    return 0;
}

static uint8_t *background;   /* 960x544 RGBA, or NULL for a grey checker */

static void save_ppm(const canvas *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6 %d %d 255\n", c->w, c->h);
    for (int i = 0; i < c->w * c->h; i++) {
        const uint8_t *p = c->rgba + i * 4;
        int checker = ((i % c->w) / 16 + (i / c->w) / 16) % 2 ? 60 : 90;
        for (int k = 0; k < 3; k++) {
            int bg = background ? background[i * 4 + k] : checker;
            fputc((p[k] * p[3] + bg * (255 - p[3])) / 255, f);
        }
    }
    fclose(f);
}

static void load_background(const char *path)
{
    uint32_t size;
    uint8_t *png = load(path, &size), *work;
    int w, h;

    if (!png || png_info(png, size, &w, &h) < 0 || w != 960 || h != 544) {
        fprintf(stderr, "%s: need a 960x544 PNG\n", path);
        free(png);
        return;
    }
    background = malloc(960 * 544 * 4);
    work = malloc((960 * 8 + 1) * 544);
    if (png_decode(png, size, background, 960 * 4, work, (960 * 8 + 1) * 544) < 0) {
        free(background);
        background = NULL;
    }
    free(work);
    free(png);
}

static int run_disc(int argc, char **argv)
{
    static sim s;
    umd_source src;
    iso_entry e;
    uint8_t *buf;
    menu_player mp = {&s, sim_read, sim_play, sim_stop, sim_resume, sim_status, sim_audio, sim_subtitle, sim_window,
                      sim_log};
    menu *m;
    double seconds = argc > 3 ? atof(argv[3]) : 30;
    const char *outdir = NULL, *state = NULL;
    char text[4096];
    FILE *sf;
    canvas c = {NULL, 960, 544, 960 * 4};

    s.ready_page = -1;
    int unsaved = 0;
    uint64_t last_save = 0;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outdir = argv[i + 1];
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            state = argv[i + 1];
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            load_background(argv[i + 1]);
    }
    if (host_source_open(&src, argv[2]) < 0 || iso_open(&s.vol, &src) < 0 ||
        iso_find(&s.vol, "UMD_VIDEO/PLAYLIST.UMD", &e) < 0 || !(buf = malloc(e.size)) ||
        iso_read(&s.vol, &e, 0, buf, e.size) != (int64_t)e.size || umdv_playlist_parse(buf, e.size, &s.pl) < 0) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    c.rgba = calloc(960 * 544, 4);
    if ((m = menu_open(&mp, getenv("MENU_LANG") ? getenv("MENU_LANG") : "en")) == NULL) {
        printf("no menus\n");
        return 1;
    }
    s.m = m;
    if (state && (sf = fopen(state, "r")) != NULL) {
        int n = 0;
        if (fgets(text, sizeof(text), sf) && sscanf(text, "%d %d %n", &s.saved_playlist, &s.saved_chapter, &n) >= 2) {
            menu_set_state(m, text + n);
            printf("    0.00 s  CONTINUE title %d chapter %d\n", s.saved_playlist, s.saved_chapter);
        }
        fclose(sf);
        if (!s.saved_playlist || !menu_continue(m))
            menu_start(m);
    } else {
        menu_start(m);
    }
    for (s.now = 0; s.now < (uint64_t)(seconds * 1e6); s.now += 16667) {
        uint32_t pressed = 0, held = 0;
        for (int i = 4; i < argc; i++) {
            const char *b = strchr(argv[i], ':');
            uint64_t at = (uint64_t)(atof(argv[i]) * 1e6);
            char hb[16];
            float hold_s;
            /* <time>:hold:<button>:<seconds> presses and holds a button */
            if (b && sscanf(b + 1, "hold:%15[a-z]:%f", hb, &hold_s) == 2) {
                if (s.now >= at && s.now < at + (uint64_t)(hold_s * 1e6))
                    held |= button(hb);
                if (s.now >= at && s.now < at + 16667) {
                    pressed |= button(hb);
                    printf("%8.2f s  hold %s for %.1f s\n", s.now / 1e6, hb, hold_s);
                }
                continue;
            }
            if (b && s.now >= at && s.now < at + 16667) {
                pressed |= button(b + 1);
                printf("%8.2f s  press %s\n", s.now / 1e6, b + 1);
                if (strcmp(b + 1, "menu") == 0)      /* Circle during playback, in src/app/disc.c */
                    menu_menu_button(m);
                {
                    int x0, y0, x1, y1;
                    if (sscanf(b + 1, "tap:%d:%d", &x0, &y0) == 2) {
                        menu_touch(m, 1, x0, y0);
                        menu_touch(m, 0, 0, 0);
                    } else if (sscanf(b + 1, "swipe:%d:%d:%d:%d", &x0, &y0, &x1, &y1) == 4) {
                        menu_touch(m, 1, x0, y0);
                        menu_touch(m, 1, x1, y1);
                        menu_touch(m, 0, 0, 0);
                    }
                }
            }
        }
        if (s.st.playing && !s.st.paused) {
            const umdv_title *t = &s.pl.titles[s.st.playlist - 1];
            uint64_t left = s.title_end > s.now ? s.title_end - s.now : 0;
            uint64_t pos = (uint64_t)(t->end_pts - t->start_pts) * 100 / 9 - left;
            int ch = 1, fired = 0, real_ch;
            for (int k = 1; k < t->chapter_count; k++)
                if ((uint64_t)(s.pl.chapters[t->first_chapter + k].pts - t->start_pts) * 100 / 9 <= pos)
                    ch = k + 1;
            real_ch = ch;
            for (int k = 0; k < t->event_count; k++) {
                const umdv_chapter *ev = &s.pl.events[t->first_event + k];
                uint64_t at = (uint64_t)(ev->pts - t->start_pts) * 100 / 9;
                if (at >= s.events_done && at <= pos) {
                    s.events_done = at + 1;
                    printf("%8.2f s  MARK %s\n", s.now / 1e6, ev->name);
                    menu_mark_reached(m, ev->name, ev->data);
                    fired = 1;               /* the handler may have changed the title: next frame */
                    break;
                }
            }
            if (fired) {
            } else if (ch != s.last_chapter && s.now >= s.title_start + 100000 &&
                       ((!menu_locked(m) && (s.last_chapter || s.now >= s.seek_until)) || s.now >= s.title_end)) {   /* as src/app/disc.c */
                if (!s.last_chapter)
                    ch = s.start_chapter;    /* the first event is the chapter it started in */
                else if (ch > s.last_chapter + 1)
                    ch = s.last_chapter + 1; /* then each chapter passed since, in order */
                const umdv_chapter *mk = &s.pl.chapters[t->first_chapter + ch - 1];
                s.last_chapter = ch;
                s.st.chapter = ch;
                snprintf(s.st.mark_name, sizeof(s.st.mark_name), "%s", mk->name);
                s.st.mark_data = mk->data;
                menu_chapter_changed(m, ch);
                fired = 1;                   /* the handler may have stopped the title */
            }
            if (!fired && s.now >= s.title_end && real_ch == s.last_chapter) {   /* every chapter reported first */
                int pl = s.st.playlist;
                printf("%8.2f s  END title %d\n", s.now / 1e6, pl);
                s.st.playing = 0;            /* playlist stays: the scripts ask which one ended */
                s.last_chapter = 0;
                menu_playlist_ended(m, pl);
            }
        }
        for (int i = 4; i < argc; i++) {
            uint64_t at = (uint64_t)(atof(argv[i]) * 1e6);
            if (strstr(argv[i], ":debug") && s.now >= at && s.now < at + 16667)
                menu_debug(m);
        }
        if (!menu_update(m, pressed, held, s.now)) {
            printf("%8.2f s  menus ended\n", s.now / 1e6);
            break;
        }
        {
            /* A page that takes input became active: tools/menu_check.sh looks for the first. */
            int ready = menu_has_focus(m) ? menu_active_page(m) : -1;
            if (ready != s.ready_page && ready >= 0)
                printf("%8.2f s  READY page %d\n", s.now / 1e6, ready);
            s.ready_page = ready;
        }
        if (menu_dirty(m)) {
            menu_draw(m, &c);
            unsaved = 1;
        }
        /* Save at most every 0.5 s, but always the latest picture. */
        if (outdir && unsaved && (s.now - last_save >= 500000 || pressed)) {
            char path[512];
            snprintf(path, sizeof(path), "%s/menu-%06llu.ppm", outdir, (unsigned long long)(s.now / 1000));
            save_ppm(&c, path);
            last_save = s.now;
            unsaved = 0;
        }
    }
    if (state && (sf = fopen(state, "w")) != NULL) {
        int n;
        if (s.st.playing && !s.menu_mode) {
            s.saved_playlist = s.st.playlist;
            s.saved_chapter = s.st.chapter;
        }
        printf("%8.2f s  EXIT\n", s.now / 1e6);
        menu_exit(m);
        n = snprintf(text, sizeof(text), "%d %d ", s.saved_playlist, s.saved_chapter);
        menu_get_state(m, text + n, (int)sizeof(text) - n);
        fprintf(sf, "%s\n", text);
        fclose(sf);
    }
    menu_close(m);
    free(c.rgba);
    return 0;
}

static int run_rco(int argc, char **argv)
{
    uint32_t size;
    uint8_t *data;
    rco r;
    int run = argc > 2 && strcmp(argv[1], "-r") == 0;

    if (run)
        argv++;
    if (argc < 2 || !(data = load(argv[1], &size)) || rco_parse(&r, data, size) < 0) {
        fprintf(stderr, "usage: menutest <file.rco>\n");
        return 1;
    }
    printf("script %u bytes, %d images, %d widgets, %d animations, %d steps\n", r.script_size, r.image_count,
           r.object_count, r.anim_count, r.step_count);
    if (run) {
        int rc = run_script(&r);
        rco_free(&r);
        free(data);
        return rc;
    }
    for (int i = 0; i < r.object_count; i++) {
        const rco_object *o = &r.objects[i];
        printf("widget %3d kind %02x %-22s parent %3d pos %g,%g scale %g alpha %g image %s", i, o->kind, nm(o->name),
               o->parent, o->pos[0], o->pos[1], o->scale[0], o->color[3], o->image >= 0 ? nm(r.images[o->image].name) : "-");
        for (int a = 0; a < RCO_BTN_ACTIONS; a++)
            if (o->action[a].event || o->action[a].object >= 0)
                printf(" [%d %s]", a, o->action[a].event ? o->action[a].event : nm(r.objects[o->action[a].object].name));
        printf("\n");
    }
    for (int i = 0; i < r.image_count; i++)
        printf("image %3d %-26s %u bytes%s\n", i, nm(r.images[i].name), r.images[i].size,
               r.images[i].data ? "" : " (no data)");
    for (int i = 0; i < r.anim_count; i++) {
        const rco_anim *a = &r.anims[i];
        printf("anim %-26s %d steps:", nm(a->name), a->step_count);
        for (int k = 0; k < a->step_count && k < 6; k++) {
            const rco_step *s = &r.steps[a->first_step + k];
            printf(" %d(%s %g)", s->kind, s->object >= 0 ? nm(r.objects[s->object].name) : nm(s->event), s->duration);
        }
        printf("\n");
    }
    rco_free(&r);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "-d") == 0)
        return run_disc(argc, argv);
    return run_rco(argc, argv);
}
