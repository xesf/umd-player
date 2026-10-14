/* Plain, CSO and ZSO disc images. See docs/formats/compressed-iso.md for the layout. */
#include "disc_image.h"

#include <stdlib.h>
#include <string.h>

#include "png.h"

#define CSO_HEADER     24
#define CSO_MAX_BLOCK  (64 * 1024)
#define CSO_READ_CHUNK (64 * 1024)    /* compressed bytes read from the file at once */

typedef enum { IMG_PLAIN, IMG_CSO, IMG_ZSO } image_kind;

typedef struct image {
    image_file file;
    image_kind kind;
    int version;
    uint32_t block_size, block_count, align;
    uint32_t *index;                  /* block_count + 1 entries */
    uint8_t *chunk;                   /* compressed bytes of several blocks */
    uint64_t chunk_off;
    uint32_t chunk_len;
    uint8_t *block;                   /* the last decompressed block */
    uint32_t block_no;                /* which block that is, or UINT32_MAX */
} image;

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int64_t lz4_block_decode(const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size)
{
    uint32_t ip = 0, op = 0;

    while (ip < in_size) {
        uint32_t token = in[ip++], len = token >> 4, match, off;
        if (len == 15) {
            uint8_t b;
            do {
                if (ip >= in_size)
                    return -1;
                b = in[ip++];
                len += b;
            } while (b == 255);
        }
        if (len > in_size - ip || len > out_size - op)
            return -1;
        memcpy(out + op, in + ip, len);
        ip += len;
        op += len;
        if (ip == in_size)
            break;                    /* the last sequence has literals only */
        if (in_size - ip < 2)
            return -1;
        off = (uint32_t)in[ip] | (uint32_t)in[ip + 1] << 8;
        ip += 2;
        if (off == 0 || off > op)
            return -1;
        match = (token & 15) + 4;
        if ((token & 15) == 15) {
            uint8_t b;
            do {
                if (ip >= in_size)
                    return -1;
                b = in[ip++];
                match += b;
            } while (b == 255);
        }
        if (match > out_size - op)
            return -1;
        for (uint32_t i = 0; i < match; i++, op++)   /* byte by byte: the match may overlap */
            out[op] = out[op - off];
    }
    return op;
}

int image_name_supported(const char *name)
{
    static const char *const ext[] = {"iso", "cso", "zso"};
    const char *dot = strrchr(name, '.');

    if (!dot || dot == name || strlen(dot) != 4)
        return 0;
    for (int i = 0; i < 3; i++) {
        int same = 1;
        for (int k = 0; k < 3; k++)
            if ((dot[1 + k] | 0x20) != ext[i][k])
                same = 0;
        if (same)
            return 1;
    }
    return 0;
}

static int plain_read(void *user, uint32_t lba, uint32_t count, void *dst)
{
    image *im = (image *)user;
    return im->file.read(im->file.user, (uint64_t)lba * UMD_SECTOR_SIZE, count * UMD_SECTOR_SIZE, dst);
}

/* Gets the compressed bytes [off, off + len) through the chunk buffer, reading ahead so that
   consecutive blocks cost one file read. */
static const uint8_t *compressed_bytes(image *im, uint64_t off, uint32_t len)
{
    uint32_t want;

    if (off >= im->chunk_off && off + len <= im->chunk_off + im->chunk_len)
        return im->chunk + (off - im->chunk_off);
    want = CSO_READ_CHUNK;
    if (want > im->file.size - off)
        want = (uint32_t)(im->file.size - off);
    if (want < len)
        return NULL;
    if (im->file.read(im->file.user, off, want, im->chunk) < 0) {
        im->chunk_len = 0;
        return NULL;
    }
    im->chunk_off = off;
    im->chunk_len = want;
    return im->chunk;
}

static int load_block(image *im, uint32_t n)
{
    uint32_t a = im->index[n], b = im->index[n + 1];
    uint64_t off = (uint64_t)(a & 0x7FFFFFFFu) << im->align, end = (uint64_t)(b & 0x7FFFFFFFu) << im->align;
    uint32_t len;
    const uint8_t *src;
    int plain, lz4;
    int64_t got;

    if (im->block_no == n)
        return 0;
    if (end < off || end > im->file.size)
        return -1;
    len = (uint32_t)(end - off);
    if (im->version >= 2) {
        /* Version 2: a block stored at full size is plain; the top bit selects LZ4. */
        plain = len >= im->block_size;
        lz4 = (a & 0x80000000u) != 0;
    } else {
        plain = (a & 0x80000000u) != 0;
        lz4 = im->kind == IMG_ZSO;
    }
    if (plain && len > im->block_size)
        len = im->block_size;         /* alignment padding follows the data */
    if (len > CSO_READ_CHUNK || (src = compressed_bytes(im, off, len)) == NULL)
        return -1;
    if (plain) {
        if (len < im->block_size)
            return -1;
        memcpy(im->block, src, im->block_size);
    } else {
        got = lz4 ? lz4_block_decode(src, len, im->block, im->block_size) : inflate_raw(src, len, im->block, im->block_size);
        if (got < 0)
            return -1;
        if ((uint32_t)got < im->block_size)
            memset(im->block + got, 0, im->block_size - (uint32_t)got);
    }
    im->block_no = n;
    return 0;
}

static int compressed_read(void *user, uint32_t lba, uint32_t count, void *dst)
{
    image *im = (image *)user;
    uint8_t *out = (uint8_t *)dst;
    uint64_t pos = (uint64_t)lba * UMD_SECTOR_SIZE, end = pos + (uint64_t)count * UMD_SECTOR_SIZE;

    while (pos < end) {
        uint32_t n = (uint32_t)(pos / im->block_size), in = (uint32_t)(pos % im->block_size);
        uint32_t take = im->block_size - in;
        if (n >= im->block_count || load_block(im, n) < 0)
            return -1;
        if (take > end - pos)
            take = (uint32_t)(end - pos);
        memcpy(out, im->block + in, take);
        out += take;
        pos += take;
    }
    return 0;
}

static void free_image(image *im)
{
    free(im->index);
    free(im->chunk);
    free(im->block);
    free(im);
}

int image_open(umd_source *out, const image_file *file)
{
    uint8_t h[CSO_HEADER];
    image *im;
    uint64_t total;

    memset(out, 0, sizeof(*out));
    if ((im = calloc(1, sizeof(*im))) == NULL)
        return -1;
    im->file = *file;
    im->block_no = UINT32_MAX;
    if (file->size < CSO_HEADER || file->read(file->user, 0, CSO_HEADER, h) < 0 ||
        (memcmp(h, "CISO", 4) != 0 && memcmp(h, "ZISO", 4) != 0)) {
        im->kind = IMG_PLAIN;
        out->user = im;
        out->read_sectors = plain_read;
        out->sector_count = (uint32_t)(file->size / UMD_SECTOR_SIZE);
        return 0;
    }
    im->kind = h[0] == 'Z' ? IMG_ZSO : IMG_CSO;
    total = (uint64_t)le32(h + 8) | (uint64_t)le32(h + 12) << 32;
    im->block_size = le32(h + 16);
    im->version = h[20];
    im->align = h[21];
    if (im->block_size < UMD_SECTOR_SIZE || im->block_size > CSO_MAX_BLOCK || im->block_size % UMD_SECTOR_SIZE ||
        im->align > 16 || total == 0 || total / im->block_size >= 0x10000000u) {
        free_image(im);
        return -1;
    }
    im->block_count = (uint32_t)((total + im->block_size - 1) / im->block_size);
    im->index = malloc(((size_t)im->block_count + 1) * 4);
    im->chunk = malloc(CSO_READ_CHUNK);
    im->block = malloc(im->block_size);
    if (!im->index || !im->chunk || !im->block ||
        file->read(file->user, CSO_HEADER, (im->block_count + 1) * 4, im->index) < 0) {
        free_image(im);
        return -1;
    }
    for (uint32_t i = 0; i <= im->block_count; i++)    /* the index is little-endian on disk */
        im->index[i] = le32((const uint8_t *)&im->index[i]);
    out->user = im;
    out->read_sectors = compressed_read;
    out->sector_count = (uint32_t)(total / UMD_SECTOR_SIZE);
    return 0;
}

void image_close(umd_source *src, image_file *file)
{
    image *im = (image *)src->user;

    memset(file, 0, sizeof(*file));
    if (!im)
        return;
    *file = im->file;
    free_image(im);
    src->user = NULL;
    src->read_sectors = NULL;
}
