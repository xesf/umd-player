/* playtest: drives the app headlessly on the null platform and checks that it keeps showing
   frames. playtest <iso | folder> <seconds> [<time>:<button>[:<hold seconds>]]...
   Buttons: up down left right cross circle square triangle l r start select, touch=x,y for
   a finger on the screen at (x, y) in 960x544 pixels, or stick=y for the left stick (-127 up
   to 127 down).
   Example: playtest umd/movie.iso 30 5:triangle 6:right 8:circle 10:up 12:right:3
   Exits with 1 if the player stops showing frames for more than 2 s (a hang). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/platform/platform.h"

extern uint32_t null_buttons, null_presents;
extern int null_touch_down, null_touch_x, null_touch_y, null_stick_y;

static uint32_t button(const char *name)
{
    static const struct { const char *n; uint32_t b; } map[] = {
        {"up", PLAT_BTN_UP}, {"down", PLAT_BTN_DOWN}, {"left", PLAT_BTN_LEFT}, {"right", PLAT_BTN_RIGHT},
        {"cross", PLAT_BTN_CROSS}, {"circle", PLAT_BTN_CIRCLE}, {"square", PLAT_BTN_SQUARE},
        {"triangle", PLAT_BTN_TRIANGLE}, {"l", PLAT_BTN_L}, {"r", PLAT_BTN_R}, {"start", PLAT_BTN_START},
        {"select", PLAT_BTN_SELECT}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(name, map[i].n) == 0)
            return map[i].b;
    return 0;
}

int main(int argc, char **argv)
{
    double seconds;
    uint64_t start, last_progress;
    uint32_t last_presents = 0;
    char *args[2];
    int hang = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: playtest <iso> <seconds> [<time>:<button>]...\n");
        return 2;
    }
    args[0] = argv[0];
    args[1] = argv[1];
    seconds = atof(argv[2]);
    if (app_init(2, args) != 0)
        return 1;
    start = last_progress = plat_time_us();
    while (plat_time_us() - start < (uint64_t)(seconds * 1e6)) {
        double t = (plat_time_us() - start) / 1e6;
        uint32_t held = 0;
        int touch = 0, stick = 0;
        for (int i = 3; i < argc; i++) {
            double at = atof(argv[i]);
            const char *b = strchr(argv[i], ':'), *h;
            char name[16];
            double hold = 0.1;
            if (!b)
                continue;
            snprintf(name, sizeof(name), "%s", b + 1);
            if ((h = strchr(b + 1, ':')) != NULL) {
                hold = atof(h + 1);
                name[h - b - 1 < (long)sizeof(name) ? h - b - 1 : (long)sizeof(name) - 1] = 0;
            }
            if (t >= at && t < at + hold) {
                if (strncmp(name, "touch=", 6) == 0 && sscanf(name + 6, "%d,%d", &null_touch_x, &null_touch_y) == 2)
                    touch = 1;
                else if (strncmp(name, "stick=", 6) == 0)
                    stick = atoi(name + 6);
                else
                    held |= button(name);
            }
        }
        null_buttons = held;
        null_touch_down = touch;
        null_stick_y = stick;
        if (!app_frame())
            break;
        if (null_presents != last_presents) {
            last_presents = null_presents;
            last_progress = plat_time_us();
        } else if (plat_time_us() - last_progress > 5000000 && !hang) {
            printf("HANG: nothing presented for 5 s at %.1f s\n", t);
            hang = 1;
        }
        usleep(16667);
    }
    app_shutdown();
    printf("%s: %u presents\n", hang ? "FAIL" : "ok", null_presents);
    return hang;
}
