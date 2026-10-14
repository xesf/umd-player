#ifndef DISC_IMAGE_H
#define DISC_IMAGE_H

#include <stdint.h>
#include "umd_source.h"

/* Disc image formats behind a umd_source: plain ISO, CSO (blocks compressed with raw DEFLATE,
   versions 1 and 2) and ZSO (blocks compressed with LZ4). The platform supplies byte reads of
   the file; this module turns them into 2048-byte sector reads. */

typedef struct image_file {
    void *user;
    /* Reads exactly len bytes at byte offset off. Returns 0 on success, negative on error. */
    int (*read)(void *user, uint64_t off, uint32_t len, void *dst);
    uint64_t size;
} image_file;

/* Detects the format from the first bytes. Returns 0 on success, negative on error or an
   unsupported image. out keeps a copy of file; free it with image_close. */
int image_open(umd_source *out, const image_file *file);

/* Frees what image_open allocated and gives back the file so the caller can close it. */
void image_close(umd_source *src, image_file *file);

/* True when the file name ends in .iso, .cso or .zso, in any case. */
int image_name_supported(const char *name);

/* Raw LZ4 block decoding. Returns the bytes written, or -1 on bad data or a full output. */
int64_t lz4_block_decode(const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size);

#endif
