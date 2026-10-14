#ifndef SFO_H
#define SFO_H

#include <stdint.h>

/* PARAM.SFO key/value table. The buffer holds the whole file and must outlive lookups. */

/* Copies a string value into out (always zero-terminated). Returns 0 if found. */
int sfo_get_string(const uint8_t *sfo, uint32_t size, const char *key, char *out, uint32_t out_size);

/* Returns 0 if found. */
int sfo_get_u32(const uint8_t *sfo, uint32_t size, const char *key, uint32_t *out);

typedef void (*sfo_each_fn)(void *user, const char *key, uint16_t fmt, const uint8_t *data, uint32_t len);
int sfo_each(const uint8_t *sfo, uint32_t size, sfo_each_fn fn, void *user);

#define SFO_FMT_UTF8_RAW 0x0004
#define SFO_FMT_UTF8     0x0204
#define SFO_FMT_U32      0x0404

#endif
