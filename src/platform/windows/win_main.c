/* Windows: window, display, input and the main loop, with the Win32 API. The picture is drawn
   into a DIB section by the shared compositor and copied to the window with GDI. Gamepads use
   XInput (xinput1_4.dll, part of Windows); touch works through the mouse messages Windows
   sends for a touchscreen.

   umdplayer.exe [folder | iso] [--fullscreen]
   Without a folder, the library is %USERPROFILE%\UMD. F11 or Alt+Enter toggles full screen. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <xinput.h>

#include "../platform.h"
#include "../soft/soft.h"
#include "../../app/library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_W 960
#define WINDOW_H 544

static HWND hwnd;
static HDC mem_dc;
static HBITMAP dib, old_bitmap;
static uint8_t *pixels;
static int win_w = WINDOW_W, win_h = WINDOW_H;
static int quit, fullscreen;
static WINDOWPLACEMENT saved_placement;
static uint32_t key_buttons;
static int touch_down, touch_x, touch_y;

/* ---- XInput, loaded at run time ---- */

typedef DWORD(WINAPI *xinput_get_state_fn)(DWORD, XINPUT_STATE *);
static xinput_get_state_fn xinput_get_state;

static void load_xinput(void)
{
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m)
        m = LoadLibraryW(L"xinput9_1_0.dll");
    if (m)
        xinput_get_state = (xinput_get_state_fn)(void (*)(void))GetProcAddress(m, "XInputGetState");
}

static uint32_t pad_buttons(void)
{
    uint32_t b = 0;
    if (!xinput_get_state)
        return 0;
    for (DWORD i = 0; i < 4; i++) {
        XINPUT_STATE s;
        WORD w;
        if (xinput_get_state(i, &s) != ERROR_SUCCESS)
            continue;
        w = s.Gamepad.wButtons;
        if (w & XINPUT_GAMEPAD_DPAD_UP) b |= PLAT_BTN_UP;
        if (w & XINPUT_GAMEPAD_DPAD_DOWN) b |= PLAT_BTN_DOWN;
        if (w & XINPUT_GAMEPAD_DPAD_LEFT) b |= PLAT_BTN_LEFT;
        if (w & XINPUT_GAMEPAD_DPAD_RIGHT) b |= PLAT_BTN_RIGHT;
        if (w & XINPUT_GAMEPAD_A) b |= PLAT_BTN_CROSS;
        if (w & XINPUT_GAMEPAD_B) b |= PLAT_BTN_CIRCLE;
        if (w & XINPUT_GAMEPAD_X) b |= PLAT_BTN_SQUARE;
        if (w & XINPUT_GAMEPAD_Y) b |= PLAT_BTN_TRIANGLE;
        if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= PLAT_BTN_L;
        if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= PLAT_BTN_R;
        if (w & XINPUT_GAMEPAD_START) b |= PLAT_BTN_START;
        if (w & XINPUT_GAMEPAD_BACK) b |= PLAT_BTN_SELECT;
        /* the left stick counts as the D-pad past half way */
        if (s.Gamepad.sThumbLX < -16384) b |= PLAT_BTN_LEFT;
        if (s.Gamepad.sThumbLX > 16384) b |= PLAT_BTN_RIGHT;
        if (s.Gamepad.sThumbLY > 16384) b |= PLAT_BTN_UP;
        if (s.Gamepad.sThumbLY < -16384) b |= PLAT_BTN_DOWN;
    }
    return b;
}

static int keys_last = 1;         /* the last input came from the keyboard */

uint32_t plat_buttons(void)
{
    uint32_t keys = key_buttons, pad = pad_buttons();
    if (pad)
        keys_last = 0;
    else if (keys)
        keys_last = 1;
    return keys | pad;
}

int plat_keyboard_ui(void)
{
    return keys_last;
}

int plat_touch(int *x, int *y)
{
    if (!touch_down)
        return 0;
    soft_touch_point(touch_x, touch_y, win_w, win_h, x, y);
    return 1;
}

/* ---- the window ---- */

static void resize_buffer(int w, int h)
{
    BITMAPINFO bi;
    HDC dc;

    if (w <= 0 || h <= 0)
        return;
    if (dib) {
        SelectObject(mem_dc, old_bitmap);
        DeleteObject(dib);
        dib = NULL;
        pixels = NULL;
    }
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;    /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = GetDC(hwnd);
    if (!mem_dc)
        mem_dc = CreateCompatibleDC(dc);
    dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    ReleaseDC(hwnd, dc);
    if (dib)
        old_bitmap = (HBITMAP)SelectObject(mem_dc, dib);
    win_w = w;
    win_h = h;
}

static void blit(HDC dc)
{
    if (dib)
        BitBlt(dc, 0, 0, win_w, win_h, mem_dc, 0, 0, SRCCOPY);
}

static void set_fullscreen(int on)
{
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    if (on && !fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        saved_placement.length = sizeof(saved_placement);
        GetWindowPlacement(hwnd, &saved_placement);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else if (!on && fullscreen) {
        SetWindowLongW(hwnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(hwnd, &saved_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen = on;
}

static uint32_t key_button(WPARAM vk)
{
    switch (vk) {
    case VK_UP: return PLAT_BTN_UP;
    case VK_DOWN: return PLAT_BTN_DOWN;
    case VK_LEFT: return PLAT_BTN_LEFT;
    case VK_RIGHT: return PLAT_BTN_RIGHT;
    case VK_SPACE: return PLAT_BTN_CROSS;
    case VK_BACK: return PLAT_BTN_CIRCLE;
    case VK_ESCAPE: return PLAT_BTN_CIRCLE;
    case 'A': return PLAT_BTN_SQUARE;
    case 'W': return PLAT_BTN_TRIANGLE;
    case 'Q': return PLAT_BTN_L;
    case 'E': return PLAT_BTN_R;
    case VK_RETURN: return PLAT_BTN_CROSS;    /* as space; the keypad Enter too */
    case VK_F1: return PLAT_BTN_HELP;
    case VK_TAB: return PLAT_BTN_SELECT;
    default: return 0;
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        plat_log("quit: window closed\n");
        quit = 1;
        return 0;
    case WM_SIZE:
        if (LOWORD(lp) && HIWORD(lp) && (LOWORD(lp) != win_w || HIWORD(lp) != win_h))
            resize_buffer(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        blit(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) {     /* Alt+Enter */
            set_fullscreen(!fullscreen);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) {
            set_fullscreen(!fullscreen);
        } else {
            key_buttons |= key_button(wp);
        }
        return 0;
    case WM_KEYUP:
        key_buttons &= ~key_button(wp);
        return 0;
    case WM_KILLFOCUS:
        key_buttons = 0;
        touch_down = 0;
        return 0;
    case WM_LBUTTONDOWN:
        touch_down = 1;
        touch_x = (short)LOWORD(lp);
        touch_y = (short)HIWORD(lp);
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE:
        if (touch_down) {
            touch_x = (short)LOWORD(lp);
            touch_y = (short)HIWORD(lp);
        }
        return 0;
    case WM_LBUTTONUP:
        touch_down = 0;
        ReleaseCapture();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void plat_present(plat_frame *frame, const plat_present_info *info)
{
    HDC dc;
    if (!pixels)
        return;
    soft_compose(pixels, win_w, win_h, win_w * 4, frame, info);
    GdiFlush();
    dc = GetDC(hwnd);
    blit(dc);
    ReleaseDC(hwnd, dc);
}

void plat_screen_size(int *w, int *h)
{
    *w = win_w;
    *h = win_h;
}

static int open_window(HINSTANCE inst)
{
    WNDCLASSW wc;
    RECT r = {0, 0, WINDOW_W, WINDOW_H};

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"UMDPlayer";
    if (!RegisterClassW(&wc))
        return -1;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowW(L"UMDPlayer", L"UMD Player", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!hwnd)
        return -1;
    resize_buffer(WINDOW_W, WINDOW_H);
    ShowWindow(hwnd, SW_SHOW);
    if (fullscreen) {
        fullscreen = 0;
        set_fullscreen(1);
    }
    return 0;
}

/* Testing aid: UMD_TEST_KEYS="2000:space,9000:escape" presses keys at those times (ms after
   start) through the window's own messages. Keys: space, escape, backspace, up, down, left,
   right, return, tab. */
static void test_keys(void)
{
    static uint64_t started;
    static int next;
    static WPARAM held;            /* released on the next frame, so the app sees it held */
    const char *spec = getenv("UMD_TEST_KEYS");
    if (!spec)
        return;
    if (held) {
        PostMessageW(hwnd, WM_KEYUP, held, 0);
        held = 0;
        return;
    }
    if (!started)
        started = plat_time_us();
    for (;;) {
        const char *p = spec;
        int k = 0;
        unsigned long long at;
        char name[16] = "";
        while (k < next && (p = strchr(p, ',')) != NULL) {
            p++;
            k++;
        }
        if (!p || sscanf(p, "%llu:%15[a-z]", &at, name) != 2 || plat_time_us() - started < at * 1000u)
            return;
        {
            static const struct { const char *n; WPARAM vk; } keys[] = {
                {"space", VK_SPACE}, {"escape", VK_ESCAPE}, {"backspace", VK_BACK}, {"up", VK_UP}, {"down", VK_DOWN},
                {"left", VK_LEFT}, {"right", VK_RIGHT}, {"return", VK_RETURN}, {"tab", VK_TAB}};
            for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
                if (strcmp(name, keys[i].n) == 0) {
                    PostMessageW(hwnd, WM_KEYDOWN, keys[i].vk, 0);
                    held = keys[i].vk;
                }
        }
        next++;
        return;
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    int wargc = 0, n = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char *args[64], *path = NULL, home_lib[MAX_PATH * 3];
    static char storage[64][MAX_PATH * 3];

    (void)prev;
    (void)cmd;
    (void)show;
    /* the command line, as UTF-8 */
    for (int i = 0; i < wargc && n < 62; i++) {
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, storage[n], (int)sizeof(storage[n]), NULL, NULL);
        if (i > 0 && strcmp(storage[n], "--fullscreen") == 0) {
            fullscreen = 1;
            continue;
        }
        /* -t and -a take a value; anything else not starting with '-' is the folder or disc */
        if (i > 0 && storage[n][0] != '-' && !path && strcmp(storage[n - 1], "-t") != 0 && strcmp(storage[n - 1], "-a") != 0)
            path = storage[n];
        args[n] = storage[n];
        n++;
    }
    LocalFree(wargv);
    if (!path) {
        /* the folder of umdplayer.exe if it holds disc images, else a UMD folder next to
           it, else UMD in the user's folder */
        const wchar_t *profile = _wgetenv(L"USERPROFILE");
        char base[MAX_PATH * 3] = ".", exe_dir[MAX_PATH * 3] = "", *slash;
        wchar_t wdir[MAX_PATH * 2], wexe[MAX_PATH * 2];
        DWORD len = GetModuleFileNameW(NULL, wexe, MAX_PATH * 2);
        if (len > 0 && len < MAX_PATH * 2) {
            WideCharToMultiByte(CP_UTF8, 0, wexe, -1, exe_dir, (int)sizeof(exe_dir), NULL, NULL);
            if ((slash = strrchr(exe_dir, '\\')) != NULL)
                *slash = 0;
        }
        snprintf(home_lib, sizeof(home_lib), "%s\\UMD", exe_dir);
        if (exe_dir[0] && library_has_discs(exe_dir)) {
            snprintf(home_lib, sizeof(home_lib), "%s", exe_dir);
        } else if (!exe_dir[0] || !library_has_discs(home_lib)) {
            if (profile)
                WideCharToMultiByte(CP_UTF8, 0, profile, -1, base, (int)sizeof(base), NULL, NULL);
            snprintf(home_lib, sizeof(home_lib), "%s\\UMD", base);
            if (MultiByteToWideChar(CP_UTF8, 0, home_lib, -1, wdir, MAX_PATH * 2) > 0)
                CreateDirectoryW(wdir, NULL);
        }
        memmove(&args[2], &args[1], (size_t)(n - 1) * sizeof(args[0]));
        args[1] = home_lib;
        n++;
    }
    args[n] = NULL;
    timeBeginPeriod(1);
    load_xinput();
    if (open_window(inst) < 0)
        return 1;
    if (app_init(n, args) != 0)
        return 1;
    while (!quit) {
        uint64_t start = plat_time_us(), took;
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT)
                quit = 1;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        test_keys();
        if (quit || !app_frame())
            break;
        took = plat_time_us() - start;
        if (took < 16000)
            Sleep((DWORD)((16667 - took) / 1000));
    }
    app_shutdown();
    timeEndPeriod(1);
    return 0;
}
