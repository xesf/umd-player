#include "sfo.h"

#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int sfo_each(const uint8_t *sfo, uint32_t size, sfo_each_fn fn, void *user)
{
    uint32_t keys, data, count, i;

    if (size < 0x14 || memcmp(sfo, "\0PSF", 4) != 0)
        return -1;
    keys = le32(&sfo[8]);
    data = le32(&sfo[12]);
    count = le32(&sfo[16]);
    if (0x14 + count * 16 > size || keys >= size || data > size)
        return -1;
    for (i = 0; i < count; i++) {
        const uint8_t *ent = &sfo[0x14 + i * 16];
        uint32_t key_off = keys + le16(&ent[0]);
        uint32_t len = le32(&ent[4]);
        uint32_t data_off = data + le32(&ent[12]);
        const char *key = (const char *)&sfo[key_off];

        if (key_off >= size || data_off > size || len > size - data_off)
            return -1;
        if (memchr(key, 0, size - key_off) == NULL)
            return -1;
        fn(user, key, le16(&ent[2]), &sfo[data_off], len);
    }
    return 0;
}

typedef struct {
    const char *key;
    const uint8_t *data;
    uint32_t len;
    uint16_t fmt;
    int found;
} lookup;

static void match(void *user, const char *key, uint16_t fmt, const uint8_t *data, uint32_t len)
{
    lookup *l = (lookup *)user;
    if (!l->found && strcmp(key, l->key) == 0) {
        l->found = 1;
        l->fmt = fmt;
        l->data = data;
        l->len = len;
    }
}

int sfo_get_string(const uint8_t *sfo, uint32_t size, const char *key, char *out, uint32_t out_size)
{
    lookup l = {key, NULL, 0, 0, 0};
    uint32_t n;

    if (out_size == 0 || sfo_each(sfo, size, match, &l) < 0 || !l.found || l.fmt == SFO_FMT_U32)
        return -1;
    n = l.len < out_size - 1 ? l.len : out_size - 1;
    memcpy(out, l.data, n);
    out[n] = 0;
    return 0;
}

int sfo_get_u32(const uint8_t *sfo, uint32_t size, const char *key, uint32_t *out)
{
    lookup l = {key, NULL, 0, 0, 0};

    if (sfo_each(sfo, size, match, &l) < 0 || !l.found || l.fmt != SFO_FMT_U32 || l.len < 4)
        return -1;
    *out = le32(l.data);
    return 0;
}
