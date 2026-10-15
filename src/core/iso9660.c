#include "iso9660.h"

#include <string.h>

#define PVD_LBA 16
#define DIR_FLAG_DIRECTORY 0x02

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_sector(iso_volume *v, uint32_t lba, uint8_t *dst)
{
    if (lba >= v->sector_count)
        return -1;
    return v->src->read_sectors(v->src->user, lba, 1, dst);
}

/* Parses one directory record. Returns 0 for a real entry, 1 for "." or "..". */
static int parse_record(const uint8_t *r, iso_entry *e)
{
    uint8_t name_len = r[32];
    const char *name = (const char *)&r[33];
    size_t n;

    e->lba = le32(&r[2]);
    e->size = le32(&r[10]);
    e->is_dir = (r[25] & DIR_FLAG_DIRECTORY) != 0;
    if (name_len == 1 && (name[0] == 0 || name[0] == 1))
        return 1;
    for (n = 0; n < name_len && name[n] != ';' && n < ISO_MAX_NAME - 1; n++)
        e->name[n] = name[n];
    /* Files without an extension are stored as "NAME." */
    if (n > 0 && e->name[n - 1] == '.')
        n--;
    e->name[n] = 0;
    return 0;
}

int iso_open(iso_volume *v, const umd_source *src)
{
    const uint8_t *p = v->sector;
    int i;

    memset(v, 0, sizeof(*v));
    v->src = src;
    v->sector_count = src->sector_count;
    if (read_sector(v, PVD_LBA, v->sector) < 0)
        return -1;
    if (p[0] != 1 || memcmp(&p[1], "CD001", 5) != 0)
        return -2;
    memcpy(v->volume_id, &p[40], 32);
    for (i = 31; i >= 0 && v->volume_id[i] == ' '; i--)
        v->volume_id[i] = 0;
    if (le32(&p[80]) < v->sector_count)
        v->sector_count = le32(&p[80]);
    parse_record(&p[156], &v->root);
    v->root.is_dir = 1;
    v->root.name[0] = 0;
    return 0;
}

static int name_eq(const char *a, const char *b, size_t blen)
{
    size_t i;
    for (i = 0; i < blen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb || ca == 0)
            return 0;
    }
    return a[blen] == 0;
}

/* Records never cross a sector boundary; a zero length byte pads to the next sector. */
static int find_in_dir(iso_volume *v, const iso_entry *dir, const char *name, size_t len, iso_entry *out)
{
    /* out may be dir itself (iso_find walks in place), and each record parsed overwrites it,
       so keep where the directory is. */
    uint32_t lba = dir->lba, sectors = (dir->size + UMD_SECTOR_SIZE - 1) / UMD_SECTOR_SIZE;
    uint32_t s;

    for (s = 0; s < sectors; s++) {
        uint32_t off = 0;
        if (read_sector(v, lba + s, v->sector) < 0)
            return -1;
        while (off < UMD_SECTOR_SIZE && v->sector[off] != 0) {
            const uint8_t *r = &v->sector[off];
            if (r[0] < 34 || off + r[0] > UMD_SECTOR_SIZE)
                return -1;
            if (parse_record(r, out) == 0 && name_eq(out->name, name, len))
                return 0;
            off += r[0];
        }
    }
    return -1;
}

int iso_find(iso_volume *v, const char *path, iso_entry *out)
{
    iso_entry cur = v->root;

    while (*path == '/')
        path++;
    while (*path) {
        const char *end = path;
        while (*end && *end != '/')
            end++;
        if (!cur.is_dir || find_in_dir(v, &cur, path, (size_t)(end - path), &cur) < 0)
            return -1;
        path = end;
        while (*path == '/')
            path++;
    }
    *out = cur;
    return 0;
}

static int walk_dir(iso_volume *v, const iso_entry *dir, char *path, size_t path_len,
                    iso_walk_fn fn, void *user, int depth)
{
    uint8_t buf[UMD_SECTOR_SIZE];
    uint32_t sectors = (dir->size + UMD_SECTOR_SIZE - 1) / UMD_SECTOR_SIZE;
    uint32_t s;

    if (depth > 16)
        return -1;
    for (s = 0; s < sectors; s++) {
        uint32_t off = 0;
        if (read_sector(v, dir->lba + s, buf) < 0)
            return -1;
        while (off < UMD_SECTOR_SIZE && buf[off] != 0) {
            const uint8_t *r = &buf[off];
            iso_entry e;
            size_t n;
            int rc;

            if (r[0] < 34 || off + r[0] > UMD_SECTOR_SIZE)
                return -1;
            off += r[0];
            if (parse_record(r, &e) != 0)
                continue;
            n = strlen(e.name);
            if (path_len + n + 2 > ISO_MAX_PATH)
                return -1;
            if (path_len)
                path[path_len] = '/';
            memcpy(&path[path_len + (path_len ? 1 : 0)], e.name, n + 1);
            if ((rc = fn(user, path, &e)) != 0)
                return rc;
            if (e.is_dir) {
                rc = walk_dir(v, &e, path, path_len + (path_len ? 1 : 0) + n, fn, user, depth + 1);
                if (rc != 0)
                    return rc;
            }
            path[path_len] = 0;
        }
    }
    return 0;
}

int iso_walk(iso_volume *v, const char *dir_path, iso_walk_fn fn, void *user)
{
    char path[ISO_MAX_PATH];
    iso_entry dir;
    size_t len;

    if (iso_find(v, dir_path, &dir) < 0 || !dir.is_dir)
        return -1;
    while (*dir_path == '/')
        dir_path++;
    len = strlen(dir_path);
    if (len >= ISO_MAX_PATH)
        return -1;
    memcpy(path, dir_path, len + 1);
    return walk_dir(v, &dir, path, len, fn, user, 0);
}

int64_t iso_read(iso_volume *v, const iso_entry *e, uint64_t off, void *dst, uint32_t len)
{
    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0;

    if (off >= e->size)
        return 0;
    if (len > e->size - off)
        len = (uint32_t)(e->size - off);
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t lba = e->lba + (uint32_t)(pos / UMD_SECTOR_SIZE);
        uint32_t in = (uint32_t)(pos % UMD_SECTOR_SIZE);
        uint32_t left = len - done;

        if (in == 0 && left >= UMD_SECTOR_SIZE) {
            uint32_t count = left / UMD_SECTOR_SIZE;
            if (lba + count > v->sector_count)
                return -1;
            if (v->src->read_sectors(v->src->user, lba, count, out + done) < 0)
                return -1;
            done += count * UMD_SECTOR_SIZE;
        } else {
            uint32_t n = UMD_SECTOR_SIZE - in;
            if (n > left)
                n = left;
            if (read_sector(v, lba, v->sector) < 0)
                return -1;
            memcpy(out + done, &v->sector[in], n);
            done += n;
        }
    }
    return done;
}
