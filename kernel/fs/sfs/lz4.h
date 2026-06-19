/* kernel/fs/sfs/lz4.h — minimal LZ4 block-format codec for SFS inline
 * compression (ADR-018 slice 4i). Standard LZ4 block format (token + literals +
 * 16-bit little-endian offset + match length, 4-byte minmatch). The decompressor
 * is fully bounds-checked: malformed input returns 0, never overruns. */
#pragma once
#include <stdint.h>

#define LZ4_HASHLOG 10u
#define LZ4_HASHSZ  (1u << LZ4_HASHLOG)   /* 1024 entries = one 4 KiB page (u32) */

/* Compress src[0..src_len) into dst (capacity dst_cap). `ht` is a caller-owned,
 * zeroed table of LZ4_HASHSZ uint32. Returns the compressed length, or 0 if it
 * does not fit in dst_cap (caller should then store the data uncompressed). */
uint32_t lz4_compress(const uint8_t *src, uint32_t src_len,
                      uint8_t *dst, uint32_t dst_cap, uint32_t *ht);

/* Decompress src[0..src_len) into dst (capacity dst_cap). Returns the
 * decompressed length, or 0 on malformed input or output overflow. */
uint32_t lz4_decompress(const uint8_t *src, uint32_t src_len,
                        uint8_t *dst, uint32_t dst_cap);
