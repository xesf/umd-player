#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>

/* Plays one title of a UMD Video ISO. Portable: uses only src/core and src/platform/platform.h.

   Threads:
   - The reader thread demuxes the clip, times every video access unit (vtiming), queues the
     access units, and decodes ATRAC3plus into a PCM ring.
   - The platform's audio callback pulls from the PCM ring. Its played-sample count is the clock.
   - The main thread (player_update) decodes video a few frames ahead and shows the frame whose
     time has come. */

typedef struct player player;

typedef struct player_options {
    const char *iso_path;
    int title;          /* -1: the longest title */
    int audio;          /* audio substream 0x00-0x0F */
} player_options;

player *player_open(const player_options *opt);
/* Call once per display refresh. Returns 0 when the title has ended. */
int player_update(player *p, uint32_t pressed);
void player_close(player *p);

#endif
