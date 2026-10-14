#ifndef ISO9660_H
#define ISO9660_H

#include <stdint.h>
#include "umd_source.h"

#define ISO_MAX_NAME 64
#define ISO_MAX_PATH 256

typedef struct iso_entry {
    uint32_t lba;
    uint32_t size;
    uint8_t is_dir;
    char name[ISO_MAX_NAME]; /* without the ";1" version suffix */
} iso_entry;

typedef struct iso_volume {
    const umd_source *src;
    iso_entry root;
    char volume_id[33];
    uint32_t sector_count;
    uint8_t sector[UMD_SECTOR_SIZE]; /* scratch for directory and partial reads */
} iso_volume;

/* Returns 0 on success, negative on error. */
int iso_open(iso_volume *v, const umd_source *src);

/* Path like "UMD_VIDEO/PLAYLIST.UMD". Case-insensitive. */
int iso_find(iso_volume *v, const char *path, iso_entry *out);

/* Callback returns nonzero to stop the walk. */
typedef int (*iso_walk_fn)(void *user, const char *path, const iso_entry *e);

/* Recursive, depth first. Pass "" or "/" for the root. */
int iso_walk(iso_volume *v, const char *dir_path, iso_walk_fn fn, void *user);

/* Reads up to len bytes at byte offset off of a file. Returns the bytes read or negative. */
int64_t iso_read(iso_volume *v, const iso_entry *e, uint64_t off, void *dst, uint32_t len);

#endif
