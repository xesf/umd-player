#ifndef UMD_SOURCE_H
#define UMD_SOURCE_H

#include <stdint.h>

#define UMD_SECTOR_SIZE 2048

/* Sector reader for a disc image. The Vita backs it with sceIo, host tools with stdio,
   so the core never touches a platform file API. */
typedef struct umd_source {
    void *user;
    /* Returns 0 on success, negative on error. */
    int (*read_sectors)(void *user, uint32_t lba, uint32_t count, void *dst);
    uint32_t sector_count;
} umd_source;

#endif
