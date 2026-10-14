/* playtest: drives the player headlessly on the null platform and checks that playback keeps
   moving. playtest <iso> <seconds> [<time>:<button>]...
   Buttons: up down left right cross circle square triangle l r start select.
   Example: playtest umd/movie.iso 30 5:triangle 6:right 8:circle 10:up
   Exits with 1 if the player stops showing frames for more than 2 s (a hang). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/app/player.h"
#include "../src/platform/platform.h"

extern uint32_t null_buttons, null_presents;

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
    player_options opt = {NULL, -1, 0};
    double seconds;
    uint64_t start, last_progress;
    uint32_t last_presents = 0, prev = 0;
    player *p;
    int hang = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: playtest <iso> <seconds> [<time>:<button>]...\n");
        return 2;
    }
    opt.iso_path = argv[1];
    seconds = atof(argv[2]);
    if (!(p = player_open(&opt)))
        return 1;
    start = last_progress = plat_time_us();
    while (plat_time_us() - start < (uint64_t)(seconds * 1e6)) {
        double t = (plat_time_us() - start) / 1e6;
        uint32_t held = 0, pressed;
        for (int i = 3; i < argc; i++) {
            double at = atof(argv[i]);
            const char *b = strchr(argv[i], ':');
            if (b && t >= at && t < at + 0.1)
                held |= button(b + 1);
        }
        null_buttons = held;
        pressed = held & ~prev;
        prev = held;
        if (!player_update(p, pressed))
            break;
        if (null_presents != last_presents) {
            last_presents = null_presents;
            last_progress = plat_time_us();
        } else if (plat_time_us() - last_progress > 2000000 && !hang) {
            printf("HANG: no frame shown for 2 s at %.1f s\n", t);
            hang = 1;
        }
        usleep(16667);
    }
    player_close(p);
    printf("%s: %u presents\n", hang ? "FAIL" : "ok", null_presents);
    return hang;
}
