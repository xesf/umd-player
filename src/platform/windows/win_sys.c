/* Windows: files, threads, time, the app's data folder, and system state, with the Win32 API.
   Paths in the app are UTF-8; they are converted to UTF-16 for the system calls. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "../platform.h"
#include "../../core/disc_image.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int to_wide(const char *s, wchar_t *out, int max)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, max) > 0 ? 0 : -1;
}

static void to_utf8(const wchar_t *s, char *out, int max)
{
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, out, max, NULL, NULL) <= 0)
        out[0] = 0;
}

static FILE *open_file(const char *path, const wchar_t *mode)
{
    wchar_t w[MAX_PATH * 2];
    if (to_wide(path, w, MAX_PATH * 2) < 0)
        return NULL;
    return _wfopen(w, mode);
}

/* ---- disc images ---- */

static int read_bytes(void *user, uint64_t off, uint32_t len, void *dst)
{
    FILE *f = (FILE *)user;
    if (_fseeki64(f, (long long)off, SEEK_SET) != 0)
        return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

int plat_iso_open(const char *path, umd_source *out)
{
    FILE *f = open_file(path, L"rb");
    image_file file;

    if (!f)
        return -1;
    _fseeki64(f, 0, SEEK_END);
    file.user = f;
    file.read = read_bytes;
    file.size = (uint64_t)_ftelli64(f);
    if (image_open(out, &file) < 0) {
        fclose(f);
        return -1;
    }
    return 0;
}

void plat_iso_close(umd_source *src)
{
    image_file file;

    if (!src->read_sectors)
        return;
    image_close(src, &file);
    fclose((FILE *)file.user);
}

int plat_list_dir(const char *dir, plat_dir_fn fn, void *user)
{
    wchar_t pattern[MAX_PATH * 2];
    char path[MAX_PATH * 2], name[MAX_PATH * 3];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    snprintf(path, sizeof(path), "%s\\*", dir);
    if (to_wide(path, pattern, MAX_PATH * 2) < 0)
        return -1;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        to_utf8(fd.cFileName, name, sizeof(name));
        if (name[0] && !plat_hidden_entry(name, is_dir))
            fn(user, name, is_dir);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

/* ---- the data folder: %APPDATA%\umdplayer ---- */

static int data_path(char *path, size_t size, const char *name)
{
    wchar_t *appdata = _wgetenv(L"APPDATA"), dir[MAX_PATH * 2];
    char base[MAX_PATH * 3];

    if (!appdata)
        return -1;
    to_utf8(appdata, base, sizeof(base));
    snprintf(path, size, "%s\\umdplayer", base);
    if (to_wide(path, dir, MAX_PATH * 2) == 0)
        CreateDirectoryW(dir, NULL);
    snprintf(path, size, "%s\\umdplayer\\%s", base, name);
    return 0;
}

int plat_data_load(const char *name, char *buf, int max)
{
    char path[MAX_PATH * 3];
    FILE *f;
    int n;

    if (data_path(path, sizeof(path), name) < 0 || !(f = open_file(path, L"rb")))
        return -1;
    n = (int)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}

int plat_file_exists(const char *path)
{
    wchar_t w[MAX_PATH * 2];
    return to_wide(path, w, MAX_PATH * 2) == 0 && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

int plat_write_file(const char *path, const char *buf, int size)
{
    char dir[MAX_PATH * 3], tmp[MAX_PATH * 3 + 8];
    wchar_t wdir[MAX_PATH * 2], wpath[MAX_PATH * 2], wtmp[MAX_PATH * 2];
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    FILE *f;

    if (back > slash)
        slash = back;
    if (slash && (size_t)(slash - path) < sizeof(dir)) {
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = 0;
        if (to_wide(dir, wdir, MAX_PATH * 2) == 0)
            CreateDirectoryW(wdir, NULL);
    }
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (!(f = open_file(tmp, L"wb")))
        return -1;
    if (fwrite(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (to_wide(path, wpath, MAX_PATH * 2) < 0 || to_wide(tmp, wtmp, MAX_PATH * 2) < 0)
        return -1;
    return MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

int plat_data_save(const char *name, const char *buf, int size)
{
    char path[MAX_PATH * 3], tmp[MAX_PATH * 3 + 8];
    wchar_t wpath[MAX_PATH * 2], wtmp[MAX_PATH * 2];
    FILE *f;

    if (data_path(path, sizeof(path), name) < 0)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (!(f = open_file(tmp, L"wb")))
        return -1;
    if (fwrite(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (to_wide(path, wpath, MAX_PATH * 2) < 0 || to_wide(tmp, wtmp, MAX_PATH * 2) < 0)
        return -1;
    return MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

/* ---- threads ---- */

struct plat_thread { HANDLE h; void (*fn)(void *); void *user; };
struct plat_mutex { CRITICAL_SECTION cs; };
struct plat_cond { CONDITION_VARIABLE cv; };

static DWORD WINAPI thread_main(LPVOID arg)
{
    plat_thread *t = (plat_thread *)arg;
    t->fn(t->user);
    return 0;
}

plat_thread *plat_thread_start(void (*fn)(void *), void *user, const char *name)
{
    plat_thread *t = calloc(1, sizeof(*t));

    (void)name;
    if (!t)
        return NULL;
    t->fn = fn;
    t->user = user;
    t->h = CreateThread(NULL, 0, thread_main, t, 0, NULL);
    if (!t->h) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(plat_thread *t)
{
    if (!t)
        return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

plat_mutex *plat_mutex_create(void)
{
    plat_mutex *m = calloc(1, sizeof(*m));
    if (m)
        InitializeCriticalSection(&m->cs);
    return m;
}

void plat_mutex_lock(plat_mutex *m) { EnterCriticalSection(&m->cs); }
void plat_mutex_unlock(plat_mutex *m) { LeaveCriticalSection(&m->cs); }

void plat_mutex_destroy(plat_mutex *m)
{
    if (!m)
        return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

plat_cond *plat_cond_create(void)
{
    plat_cond *c = calloc(1, sizeof(*c));
    if (c)
        InitializeConditionVariable(&c->cv);
    return c;
}

void plat_cond_wait(plat_cond *c, plat_mutex *m) { SleepConditionVariableCS(&c->cv, &m->cs, INFINITE); }
void plat_cond_broadcast(plat_cond *c) { WakeAllConditionVariable(&c->cv); }

void plat_cond_destroy(plat_cond *c)
{
    free(c);
}

uint64_t plat_time_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000u +
           (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000u / (uint64_t)freq.QuadPart;
}

/* ---- logging and system state ---- */

void plat_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    OutputDebugStringA(buf);
    {
        /* and to log.txt in the data folder, started anew each run */
        static FILE *file;
        static int tried;
        if (!tried) {
            char path[MAX_PATH * 3];
            tried = 1;
            if (data_path(path, sizeof(path), "log.txt") == 0)
                file = open_file(path, L"w");
        }
        if (file) {
            fputs(buf, file);
            fflush(file);
        }
    }
}

void plat_keep_awake(void)
{
    SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
}

void plat_set_power(int mode)
{
    (void)mode;
}

/* UMD_LANG, else the user's locale ("fr-FR"), else English. */
void plat_language(char out[3])
{
    const char *l = getenv("UMD_LANG");
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    char loc[LOCALE_NAME_MAX_LENGTH * 3];

    if ((!l || !*l) && GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
        to_utf8(name, loc, sizeof(loc));
        l = loc;
    }
    if (l && l[0] >= 'a' && l[0] <= 'z' && l[1] >= 'a' && l[1] <= 'z') {
        out[0] = l[0];
        out[1] = l[1];
    } else {
        out[0] = 'e';
        out[1] = 'n';
    }
    out[2] = 0;
}

void plat_get_status(plat_status *s)
{
    SYSTEMTIME t;
    SYSTEM_POWER_STATUS p;

    GetLocalTime(&t);
    s->year = t.wYear;
    s->month = t.wMonth;
    s->day = t.wDay;
    s->hour = t.wHour;
    s->minute = t.wMinute;
    s->battery = -1;
    s->charging = 0;
    if (GetSystemPowerStatus(&p) && p.BatteryFlag != 128 && p.BatteryFlag != 255 && p.BatteryLifePercent <= 100) {
        s->battery = p.BatteryLifePercent;
        s->charging = p.ACLineStatus == 1;
    }
}
