/* Vita: files, threads, and time. */
#include "../platform.h"
#include "vita_source.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/apputil.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <psp2/rtc.h>

#define THREAD_PRIORITY 0x10000100
#define THREAD_STACK    (256 * 1024)
#define COND_POLL_US    2000

int plat_iso_open(const char *path, umd_source *out)
{
    return vita_source_open(out, path);
}

void plat_iso_close(umd_source *src)
{
    vita_source_close(src);
}

struct plat_thread { SceUID id; void (*fn)(void *); void *user; };
struct plat_mutex { SceKernelLwMutexWork work; };
struct plat_cond { SceUID sema; };

static int thread_entry(SceSize args, void *argp)
{
    plat_thread *t = *(plat_thread **)argp;
    (void)args;
    t->fn(t->user);
    return 0;
}

plat_thread *plat_thread_start(void (*fn)(void *), void *user, const char *name)
{
    plat_thread *t = calloc(1, sizeof(*t));

    if (!t)
        return NULL;
    t->fn = fn;
    t->user = user;
    t->id = sceKernelCreateThread(name, thread_entry, THREAD_PRIORITY, THREAD_STACK, 0, 0, NULL);
    if (t->id < 0 || sceKernelStartThread(t->id, sizeof(t), &t) < 0) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(plat_thread *t)
{
    if (!t)
        return;
    sceKernelWaitThreadEnd(t->id, NULL, NULL);
    sceKernelDeleteThread(t->id);
    free(t);
}

plat_mutex *plat_mutex_create(void)
{
    plat_mutex *m = memalign(8, sizeof(*m));
    if (m && sceKernelCreateLwMutex(&m->work, "umd", 0, 0, NULL) < 0) {
        free(m);
        return NULL;
    }
    return m;
}

void plat_mutex_lock(plat_mutex *m) { sceKernelLockLwMutex(&m->work, 1, NULL); }
void plat_mutex_unlock(plat_mutex *m) { sceKernelUnlockLwMutex(&m->work, 1); }

void plat_mutex_destroy(plat_mutex *m)
{
    if (!m)
        return;
    sceKernelDeleteLwMutex(&m->work);
    free(m);
}

/* A condition variable as a semaphore with a short timeout. Callers always re-check their
   condition in a loop, so a missed or spurious wake-up only costs one poll interval. */
plat_cond *plat_cond_create(void)
{
    plat_cond *c = calloc(1, sizeof(*c));
    if (c && (c->sema = sceKernelCreateSema("umd", 0, 0, 1, NULL)) < 0) {
        free(c);
        return NULL;
    }
    return c;
}

void plat_cond_wait(plat_cond *c, plat_mutex *m)
{
    SceUInt timeout = COND_POLL_US;

    plat_mutex_unlock(m);
    sceKernelWaitSema(c->sema, 1, &timeout);
    plat_mutex_lock(m);
}

void plat_cond_broadcast(plat_cond *c)
{
    sceKernelSignalSema(c->sema, 1);
}

void plat_cond_destroy(plat_cond *c)
{
    if (!c)
        return;
    sceKernelDeleteSema(c->sema);
    free(c);
}

uint64_t plat_time_us(void)
{
    return sceKernelGetProcessTimeWide();
}

/* Opens and closes the log on every line, so it survives the app being killed. */
/* Log lines collect here and a thread writes them every half second: opening, appending to
   and closing the file on the memory card for each line took about 10 ms, a dropped frame
   for each line the menus or the library logged. */
#define LOG_BUF  (64 * 1024)
#define LOG_FILE "ux0:data/umdplayer/log.txt"

static char log_buf[LOG_BUF];
static int log_len, log_dropped;
static SceKernelLwMutexWork log_lock;
static int log_started;

static void log_writer(void *user)
{
    static char out[LOG_BUF];
    (void)user;
    for (;;) {
        int n, dropped;
        sceKernelDelayThread(500000);
        sceKernelLockLwMutex(&log_lock, 1, NULL);
        n = log_len;
        dropped = log_dropped;
        memcpy(out, log_buf, (size_t)n);
        log_len = log_dropped = 0;
        sceKernelUnlockLwMutex(&log_lock, 1);
        if (dropped)
            n += snprintf(out + n, sizeof(out) - (size_t)n, "log: %d lines dropped\n", dropped);
        if (n > 0) {
            SceUID fd = sceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
            if (fd >= 0) {
                sceIoWrite(fd, out, n > LOG_BUF ? LOG_BUF : n);
                sceIoClose(fd);
            }
        }
    }
}

void plat_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(line))
        n = sizeof(line) - 1;
    if (!log_started) {
        log_started = 1;
        sceKernelCreateLwMutex(&log_lock, "log", 0, 0, NULL);
        if (!plat_thread_start(log_writer, NULL, "log"))
            log_started = 2;      /* no thread: write each line as before */
    }
    if (log_started == 2) {
        SceUID fd = sceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        if (fd >= 0) {
            sceIoWrite(fd, line, n);
            sceIoClose(fd);
        }
        return;
    }
    sceKernelLockLwMutex(&log_lock, 1, NULL);
    if (log_len + n <= LOG_BUF - 64) {
        memcpy(log_buf + log_len, line, (size_t)n);
        log_len += n;
    } else {
        log_dropped++;
    }
    sceKernelUnlockLwMutex(&log_lock, 1);
}

void plat_keep_awake(void)
{
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
}

void plat_language(char out[3])
{
    /* SceSystemParamLang order: Japanese, English (US), French, Spanish, German, Italian, Dutch,
       Portuguese (PT), Russian, Korean, Chinese (traditional, simplified), Finnish, Swedish,
       Danish, Norwegian, Polish, Portuguese (BR), English (GB), Turkish. */
    static const char codes[][3] = {"ja", "en", "fr", "es", "de", "it", "nl", "pt", "ru", "ko", "zh",
                                    "zh", "fi", "sv", "da", "no", "pl", "pt", "en", "tr"};
    static int inited;
    int lang = 1;

    if (!inited) {
        SceAppUtilInitParam init;
        SceAppUtilBootParam boot;
        memset(&init, 0, sizeof(init));
        memset(&boot, 0, sizeof(boot));
        sceAppUtilInit(&init, &boot);
        inited = 1;
    }
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang) < 0 || lang < 0 ||
        lang >= (int)(sizeof(codes) / sizeof(codes[0])))
        lang = 1;
    memcpy(out, codes[lang], 3);
}

void plat_set_power(int mode)
{
    static int current = -1;
    int mhz = mode == PLAT_POWER_PLAYBACK ? 333 : 444;

    if (mode == current)
        return;
    current = mode;
    scePowerSetArmClockFrequency(mhz);
    plat_log("cpu: %d MHz\n", scePowerGetArmClockFrequency());
}

void plat_get_status(plat_status *s)
{
    SceDateTime t;

    if (sceRtcGetCurrentClockLocalTime(&t) < 0)
        memset(&t, 0, sizeof(t));
    s->year = t.year;
    s->month = t.month;
    s->day = t.day;
    s->hour = t.hour;
    s->minute = t.minute;
    s->battery = scePowerGetBatteryLifePercent();
    s->charging = scePowerIsBatteryCharging() ? 1 : 0;
}

int plat_list_dir(const char *dir, plat_dir_fn fn, void *user)
{
    SceIoDirent ent;
    SceUID d = sceIoDopen(dir);

    if (d < 0)
        return -1;
    while (sceIoDread(d, &ent) > 0)
        if (ent.d_name[0] != '.')
            fn(user, ent.d_name, SCE_S_ISDIR(ent.d_stat.st_mode));
    sceIoDclose(d);
    return 0;
}

int plat_data_load(const char *name, char *buf, int max)
{
    char path[256];
    SceUID fd;
    int n;

    snprintf(path, sizeof(path), "ux0:data/umdplayer/%s", name);
    if ((fd = sceIoOpen(path, SCE_O_RDONLY, 0)) < 0)
        return -1;
    n = sceIoRead(fd, buf, (SceSize)max);
    sceIoClose(fd);
    return n;
}

int plat_file_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

int plat_write_file(const char *path, const char *buf, int size)
{
    char dir[512], tmp[528];
    const char *slash = strrchr(path, '/');
    SceUID fd;
    int n;

    if (slash && (size_t)(slash - path) < sizeof(dir)) {
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = 0;
        sceIoMkdir(dir, 0777);
    }
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)) < 0)
        return -1;
    n = sceIoWrite(fd, buf, (SceSize)size);
    sceIoClose(fd);
    if (n != size)
        return -1;
    sceIoRemove(path);
    return sceIoRename(tmp, path) < 0 ? -1 : 0;
}

int plat_data_save(const char *name, const char *buf, int size)
{
    char path[256], tmp[260];
    SceUID fd;
    int n;

    sceIoMkdir("ux0:data/umdplayer", 0777);
    snprintf(path, sizeof(path), "ux0:data/umdplayer/%s", name);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666)) < 0)
        return -1;
    n = sceIoWrite(fd, buf, (SceSize)size);
    sceIoClose(fd);
    if (n != size)
        return -1;
    sceIoRemove(path);   /* write a new file, then swap, so a crash never leaves half a file */
    return sceIoRename(tmp, path) < 0 ? -1 : 0;
}
