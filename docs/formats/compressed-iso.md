# Compressed disc images: CSO and ZSO

The player reads `.iso`, `.cso` and `.zso` files. `src/core/disc_image.c` detects the format from the first 4 bytes, not the file name. Every multi-byte field is little-endian.

## Header (24 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic: `CISO` for CSO, `ZISO` for ZSO |
| 4 | 4 | Header size, normally 24 (some tools write 0) |
| 8 | 8 | Size of the uncompressed image in bytes |
| 16 | 4 | Block size in bytes, normally 2048 |
| 20 | 1 | Version: 1, or 2 for CSO version 2 |
| 21 | 1 | Index alignment: block offsets are stored shifted right by this many bits |
| 22 | 2 | Reserved |

## Block index

The index starts at byte 24 and has one 32-bit entry per block, plus one entry for the end of the last block. The low 31 bits, shifted left by the alignment, give the block's file offset. The block's stored size is the next entry's offset minus its own.

The top bit means different things by format:

| Format | Top bit set | Top bit clear |
|---|---|---|
| CSO version 1 | Block is stored uncompressed | Raw DEFLATE (RFC 1951) |
| ZSO | Block is stored uncompressed | Raw LZ4 block |
| CSO version 2 | LZ4 | DEFLATE |

In CSO version 2, a block whose stored size is at least the block size is uncompressed, whatever the top bit says.

With alignment, padding can follow a block, so an uncompressed block is read for its first `block size` bytes only.

## How the player reads them

- The reader keeps the last decompressed block, so the 2048-byte reads from `iso_read` cost one decompression per block.
- It reads compressed data 64 KiB at a time, so a run of consecutive blocks costs one file read.
- UMD Video streams barely compress. In a test CSO of *Not Another Teen Movie*, 8,922 of 685,760 blocks were compressed, so most reads are plain copies.

## Testing

`tools/mkcso.py in.iso out.cso` writes a CSO version 1 file. `out.zso` writes a ZSO with 64 KiB blocks and needs the `lz4` command. Both outputs read back identical to the source ISO, sector by sector.
