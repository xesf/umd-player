#ifndef SFX_H
#define SFX_H

#include <stdint.h>

/* Menu sound effects, mixed into the player's audio: 48 kHz stereo. */

/* Starts a sound; the samples are copied. A full set of voices drops the oldest. */
void sfx_play(const int16_t *stereo, int frames);
void sfx_stop(void);
/* Adds the sounds playing into out (audio thread). */
void sfx_mix(int16_t *out, int frames);

#endif
