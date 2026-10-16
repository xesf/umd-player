/* Linux: window, display, input and the main loop, with X11 (libX11; Wayland desktops and
   SteamOS's gamescope run it through XWayland). Gamepads come from the kernel's evdev devices:
   the Steam Deck's controls and a Steam Machine's controllers appear there as an Xbox-style
   pad when Steam runs the app. A touchscreen reaches X11 as the pointer, so touch works
   through the pointer events.

   umdplayer [folder | iso] [--fullscreen | --window]
   Without a folder, the library is ~/UMD. Full screen is the default under SteamOS. */
#define _GNU_SOURCE
#include "../platform.h"
#include "../soft/soft.h"
#include "../../app/library.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WINDOW_W 960
#define WINDOW_H 544
#define MAX_PADS 4

static Display *dpy;
static Window win;
static GC gc;
static Visual *visual;
static int depth;
static Atom wm_delete;
static XImage *image;
static uint8_t *pixels;
static int win_w = WINDOW_W, win_h = WINDOW_H;
static int fullscreen, quit;
static uint32_t key_buttons;
static int touch_down, touch_x, touch_y;

/* ---- gamepads ---- */

typedef struct pad {
    int fd;
    uint32_t buttons;              /* from buttons */
    uint32_t hat, stick;           /* D-pad from the hat and the left stick */
    int stick_x, stick_y;
    int min_x, max_x, min_y, max_y;
} pad;

static pad pads[MAX_PADS];
static int pad_count;
static uint64_t pads_scanned_us;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

#define TEST_BIT(bits, n) (((bits)[(n) / (8 * sizeof(long))] >> ((n) % (8 * sizeof(long)))) & 1)

static void scan_pads(void)
{
    pads_scanned_us = now_us();
    for (int i = 0; i < 32 && pad_count < MAX_PADS; i++) {
        char path[64];
        unsigned long keys[(KEY_MAX + 1) / (8 * sizeof(long)) + 1];
        int fd, known = 0;
        struct stat st;
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (stat(path, &st) != 0)
            continue;
        for (int k = 0; k < pad_count; k++) {
            struct stat ps;
            if (fstat(pads[k].fd, &ps) == 0 && ps.st_rdev == st.st_rdev)
                known = 1;
        }
        if (known || (fd = open(path, O_RDONLY | O_NONBLOCK)) < 0)
            continue;
        memset(keys, 0, sizeof(keys));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !TEST_BIT(keys, BTN_SOUTH)) {
            close(fd);
            continue;
        }
        {
            pad *p = &pads[pad_count++];
            struct input_absinfo ax, ay;
            char name[128] = "";
            memset(p, 0, sizeof(*p));
            p->fd = fd;
            p->min_x = p->min_y = -32768;
            p->max_x = p->max_y = 32767;
            if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0) {
                p->min_x = ax.minimum;
                p->max_x = ax.maximum;
            }
            if (ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0) {
                p->min_y = ay.minimum;
                p->max_y = ay.maximum;
            }
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            plat_log("gamepad: %s (%s)\n", name, path);
        }
    }
}

static uint32_t pad_key(int code)
{
    switch (code) {
    case BTN_SOUTH: return PLAT_BTN_CROSS;        /* A */
    case BTN_EAST: return PLAT_BTN_CIRCLE;        /* B */
    case BTN_NORTH: return PLAT_BTN_SQUARE;       /* code 0x133: X, the left button, on Xbox-style pads */
    case BTN_WEST: return PLAT_BTN_TRIANGLE;      /* code 0x134: Y, the top button */
    case BTN_TL: return PLAT_BTN_L;
    case BTN_TR: return PLAT_BTN_R;
    case BTN_START: return PLAT_BTN_START;
    case BTN_SELECT: return PLAT_BTN_SELECT;
    case BTN_DPAD_UP: return PLAT_BTN_UP;
    case BTN_DPAD_DOWN: return PLAT_BTN_DOWN;
    case BTN_DPAD_LEFT: return PLAT_BTN_LEFT;
    case BTN_DPAD_RIGHT: return PLAT_BTN_RIGHT;
    default: return 0;
    }
}

/* The left stick counts as the D-pad past half way. */
static uint32_t stick_dirs(const pad *p)
{
    uint32_t b = 0;
    int cx = (p->min_x + p->max_x) / 2, cy = (p->min_y + p->max_y) / 2;
    int rx = (p->max_x - p->min_x) / 4, ry = (p->max_y - p->min_y) / 4;
    if (p->stick_x < cx - rx) b |= PLAT_BTN_LEFT;
    if (p->stick_x > cx + rx) b |= PLAT_BTN_RIGHT;
    if (p->stick_y < cy - ry) b |= PLAT_BTN_UP;
    if (p->stick_y > cy + ry) b |= PLAT_BTN_DOWN;
    return b;
}

static void poll_pads(void)
{
    if (pad_count == 0 && now_us() - pads_scanned_us > 2000000)
        scan_pads();
    for (int i = 0; i < pad_count; i++) {
        pad *p = &pads[i];
        struct input_event ev;
        ssize_t n;
        while ((n = read(p->fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY) {
                uint32_t b = pad_key(ev.code);
                if (ev.value)
                    p->buttons |= b;
                else
                    p->buttons &= ~b;
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_HAT0X)
                    p->hat = (p->hat & ~(PLAT_BTN_LEFT | PLAT_BTN_RIGHT)) |
                             (ev.value < 0 ? PLAT_BTN_LEFT : ev.value > 0 ? PLAT_BTN_RIGHT : 0);
                else if (ev.code == ABS_HAT0Y)
                    p->hat = (p->hat & ~(PLAT_BTN_UP | PLAT_BTN_DOWN)) |
                             (ev.value < 0 ? PLAT_BTN_UP : ev.value > 0 ? PLAT_BTN_DOWN : 0);
                else if (ev.code == ABS_X)
                    p->stick_x = ev.value;
                else if (ev.code == ABS_Y)
                    p->stick_y = ev.value;
                p->stick = stick_dirs(p);
            }
        }
        if (n < 0 && errno != EAGAIN) {
            /* unplugged */
            close(p->fd);
            pads[i] = pads[--pad_count];
            i--;
        }
    }
}

uint32_t plat_buttons(void)
{
    uint32_t b = key_buttons;
    for (int i = 0; i < pad_count; i++)
        b |= pads[i].buttons | pads[i].hat | pads[i].stick;
    return b;
}

int plat_touch(int *x, int *y)
{
    if (!touch_down)
        return 0;
    soft_touch_point(touch_x, touch_y, win_w, win_h, x, y);
    return 1;
}

/* ---- the window ---- */

static void set_fullscreen(int on)
{
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.window = win;
    e.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
    e.xclient.format = 32;
    e.xclient.data.l[0] = on ? 1 : 0;   /* _NET_WM_STATE_ADD or _REMOVE */
    e.xclient.data.l[1] = (long)XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    e.xclient.data.l[3] = 1;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    fullscreen = on;
}

static void resize_buffer(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (image) {
        image->data = NULL;        /* the buffer is ours */
        XDestroyImage(image);
        image = NULL;
    }
    free(pixels);
    win_w = w;
    win_h = h;
    pixels = calloc((size_t)w * h, 4);
    if (pixels)
        image = XCreateImage(dpy, visual, (unsigned)depth, ZPixmap, 0, (char *)pixels, (unsigned)w, (unsigned)h, 32, w * 4);
}

static uint32_t key_button(KeySym k)
{
    switch (k) {
    case XK_Up: return PLAT_BTN_UP;
    case XK_Down: return PLAT_BTN_DOWN;
    case XK_Left: return PLAT_BTN_LEFT;
    case XK_Right: return PLAT_BTN_RIGHT;
    case XK_space: return PLAT_BTN_CROSS;
    case XK_BackSpace: return PLAT_BTN_CIRCLE;
    case XK_a: return PLAT_BTN_SQUARE;
    case XK_w: return PLAT_BTN_TRIANGLE;
    case XK_q: return PLAT_BTN_L;
    case XK_e: return PLAT_BTN_R;
    case XK_Return: return PLAT_BTN_START;
    case XK_Tab: return PLAT_BTN_SELECT;
    default: return 0;
    }
}

static void handle_events(void)
{
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        switch (e.type) {
        case KeyPress:
        case KeyRelease: {
            KeySym k = XLookupKeysym(&e.xkey, 0);
            if (e.type == KeyPress && k == XK_Escape) {
                plat_log("quit: escape key\n");
                quit = 1;
            } else if (e.type == KeyPress && k == XK_F11) {
                set_fullscreen(!fullscreen);
            } else if (e.type == KeyPress) {
                key_buttons |= key_button(k);
            } else {
                key_buttons &= ~key_button(k);
            }
            break;
        }
        case ButtonPress:
            if (e.xbutton.button == Button1) {
                touch_down = 1;
                touch_x = e.xbutton.x;
                touch_y = e.xbutton.y;
            }
            break;
        case ButtonRelease:
            if (e.xbutton.button == Button1)
                touch_down = 0;
            break;
        case MotionNotify:
            if (touch_down) {
                touch_x = e.xmotion.x;
                touch_y = e.xmotion.y;
            }
            break;
        case FocusOut:
            key_buttons = 0;       /* keys released in another window never reach us */
            touch_down = 0;
            break;
        case ConfigureNotify:
            if (e.xconfigure.width != win_w || e.xconfigure.height != win_h)
                resize_buffer(e.xconfigure.width, e.xconfigure.height);
            break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == wm_delete) {
                plat_log("quit: window closed\n");
                quit = 1;
            }
            break;
        default:
            break;
        }
    }
}

void plat_present(plat_frame *frame, const plat_present_info *info)
{
    if (!image)
        return;
    soft_compose(pixels, win_w, win_h, win_w * 4, frame, info);
    XPutImage(dpy, win, gc, image, 0, 0, 0, 0, (unsigned)win_w, (unsigned)win_h);
    XFlush(dpy);
}

/* Keeps the screen saver from blanking the screen during a movie. */
void plat_keep_awake(void)
{
    if (dpy)
        XResetScreenSaver(dpy);
}

void plat_screen_size(int *w, int *h)
{
    *w = win_w;
    *h = win_h;
}

static int open_window(void)
{
    XSetWindowAttributes attrs;
    XSizeHints hints;

    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "cannot open the X display (is DISPLAY set?)\n");
        return -1;
    }
    visual = DefaultVisual(dpy, DefaultScreen(dpy));
    depth = DefaultDepth(dpy, DefaultScreen(dpy));
    if (depth != 24 && depth != 32) {
        fprintf(stderr, "a 24-bit display is needed (this one is %d-bit)\n", depth);
        return -1;
    }
    memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixel = BlackPixel(dpy, DefaultScreen(dpy));
    attrs.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       StructureNotifyMask | ExposureMask | FocusChangeMask;
    win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, WINDOW_W, WINDOW_H, 0, depth, InputOutput, visual,
                        CWBackPixel | CWEventMask, &attrs);
    XStoreName(dpy, win, "UMD Player");
    {
        XClassHint cls = {"umdplayer", "UMD Player"};
        XSetClassHint(dpy, win, &cls);
    }
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize;
    hints.min_width = 480;
    hints.min_height = 272;
    XSetWMNormalHints(dpy, win, &hints);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XkbSetDetectableAutoRepeat(dpy, True, NULL);   /* held keys stay pressed, without release events */
    gc = XCreateGC(dpy, win, 0, NULL);
    XMapWindow(dpy, win);
    resize_buffer(WINDOW_W, WINDOW_H);
    if (fullscreen)
        set_fullscreen(1);
    XFlush(dpy);
    return 0;
}

int main(int argc, char **argv)
{
    char *args[64], home_lib[1024];
    int n = 0;
    const char *path = NULL;

    /* under SteamOS (Game Mode or the desktop), full screen unless --window */
    fullscreen = getenv("SteamDeck") != NULL || getenv("SteamOS") != NULL || getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL;
    args[n++] = argv[0];
    for (int i = 1; i < argc && n < 62; i++) {
        if (strcmp(argv[i], "--fullscreen") == 0)
            fullscreen = 1;
        else if (strcmp(argv[i], "--window") == 0)
            fullscreen = 0;
        else {
            /* -t and -a take a value; anything else not starting with '-' is the folder or disc */
            if (argv[i][0] != '-' && !path && strcmp(argv[i - 1], "-t") != 0 && strcmp(argv[i - 1], "-a") != 0)
                path = argv[i];
            args[n++] = argv[i];
        }
    }
    if (!path) {
        /* the app's folder if it holds disc images, else a UMD folder next to it, else ~/UMD */
        char exe_dir[1024] = "", *slash;
        ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
        exe_dir[len > 0 ? len : 0] = 0;
        if ((slash = strrchr(exe_dir, '/')) != NULL)
            *slash = 0;
        snprintf(home_lib, sizeof(home_lib), "%s/UMD", exe_dir);
        if (exe_dir[0] && library_has_discs(exe_dir)) {
            snprintf(home_lib, sizeof(home_lib), "%s", exe_dir);
        } else if (!exe_dir[0] || !library_has_discs(home_lib)) {
            const char *home = getenv("HOME");
            snprintf(home_lib, sizeof(home_lib), "%s/UMD", home ? home : ".");
            mkdir(home_lib, 0755);
        }
        /* the folder goes first, as app_init expects */
        memmove(&args[2], &args[1], (size_t)(n - 1) * sizeof(args[0]));
        args[1] = home_lib;
        n++;
    }
    args[n] = NULL;
    if (open_window() < 0)
        return 1;
    scan_pads();
    if (app_init(n, args) != 0)
        return 1;
    while (!quit) {
        uint64_t start = now_us(), took;
        handle_events();
        poll_pads();
        if (quit || !app_frame())
            break;
        took = now_us() - start;
        if (took < 16667) {
            struct timespec ts = {0, (long)(16667 - took) * 1000};
            nanosleep(&ts, NULL);
        }
    }
    app_shutdown();
    XCloseDisplay(dpy);
    return 0;
}
