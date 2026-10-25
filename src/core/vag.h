#ifndef VAG_H
#define VAG_H

#include <stdint.h>

/* Decoder for VAG sounds: the PSP's 4-bit ADPCM, one channel per file. RCO files keep their
   menu sound effects this way. */

/* Decodes a VAG file ("VAGp" header) to 16-bit samples. Returns malloc'd samples and sets
   *count and *rate, or NULL. */
int16_t *vag_decode(const uint8_t *vag, uint32_t size, int *count, int *rate);

#endif
