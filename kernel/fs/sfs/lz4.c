/* kernel/fs/sfs/lz4.c — minimal LZ4 block-format codec (see lz4.h). */
#include "lz4.h"

#define LZ4_MINMATCH 4u
#define LZ4_LASTLIT  12u                 /* no matches within the last 12 bytes */

static inline uint32_t lz4_hash(uint32_t seq) {
    return (seq * 2654435761u) >> (32 - LZ4_HASHLOG);
}
static inline uint32_t lz4_read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Emit a length using LZ4's 255-extension scheme (the first 15 lives in the
 * token). Returns 0 on overflow. */
static int lz4_emit_len(uint8_t **op, uint8_t *oend, uint32_t len) {
    uint32_t r = len - 15;
    while (r >= 255) {
        if (*op >= oend) return 0;
        *(*op)++ = 255;
        r -= 255;
    }
    if (*op >= oend) return 0;
    *(*op)++ = (uint8_t)r;
    return 1;
}

uint32_t lz4_compress(const uint8_t *src, uint32_t src_len,
                      uint8_t *dst, uint32_t dst_cap, uint32_t *ht) {
    const uint8_t *ip = src, *anchor = src, *iend = src + src_len;
    uint8_t *op = dst, *oend = dst + dst_cap;

    if (src_len >= LZ4_LASTLIT + LZ4_MINMATCH) {
        const uint8_t *mflimit = iend - LZ4_LASTLIT;
        while (ip < mflimit) {
            uint32_t h = lz4_hash(lz4_read32(ip));
            uint32_t cand = ht[h];                     /* stored as pos + 1 */
            ht[h] = (uint32_t)(ip - src) + 1;
            if (cand) {
                const uint8_t *m = src + (cand - 1);
                uint32_t offset = (uint32_t)(ip - m);
                if (offset <= 65535 && lz4_read32(m) == lz4_read32(ip)) {
                    const uint8_t *cp = ip + LZ4_MINMATCH, *mp = m + LZ4_MINMATCH;
                    while (cp < iend && *cp == *mp) { cp++; mp++; }
                    uint32_t matchlen = (uint32_t)(cp - ip);   /* >= 4 */
                    uint32_t litlen = (uint32_t)(ip - anchor);
                    uint32_t mext = matchlen - LZ4_MINMATCH;

                    if (op >= oend) return 0;
                    *op++ = (uint8_t)((litlen < 15 ? litlen : 15) << 4 |
                                      (mext   < 15 ? mext   : 15));
                    if (litlen >= 15 && !lz4_emit_len(&op, oend, litlen)) return 0;
                    if (litlen > (uint32_t)(oend - op)) return 0;
                    for (uint32_t i = 0; i < litlen; i++) *op++ = anchor[i];
                    if (op + 2 > oend) return 0;
                    *op++ = (uint8_t)(offset & 0xFF);
                    *op++ = (uint8_t)(offset >> 8);
                    if (mext >= 15 && !lz4_emit_len(&op, oend, mext)) return 0;

                    ip += matchlen;
                    anchor = ip;
                    continue;
                }
            }
            ip++;
        }
    }

    /* Last sequence: literals only (anchor..iend). */
    uint32_t litlen = (uint32_t)(iend - anchor);
    if (op >= oend) return 0;
    *op++ = (uint8_t)((litlen < 15 ? litlen : 15) << 4);
    if (litlen >= 15 && !lz4_emit_len(&op, oend, litlen)) return 0;
    if (litlen > (uint32_t)(oend - op)) return 0;
    for (uint32_t i = 0; i < litlen; i++) *op++ = anchor[i];

    return (uint32_t)(op - dst);
}

uint32_t lz4_decompress(const uint8_t *src, uint32_t src_len,
                        uint8_t *dst, uint32_t dst_cap) {
    const uint8_t *ip = src, *iend = src + src_len;
    uint8_t *op = dst, *oend = dst + dst_cap;

    while (ip < iend) {
        uint8_t token = *ip++;
        uint32_t litlen = token >> 4;
        if (litlen == 15) {
            uint8_t b;
            do { if (ip >= iend) return 0; b = *ip++; litlen += b; } while (b == 255);
        }
        if (litlen > (uint32_t)(iend - ip) || litlen > (uint32_t)(oend - op))
            return 0;
        for (uint32_t i = 0; i < litlen; i++) *op++ = *ip++;

        if (ip >= iend) break;                         /* last sequence */
        if (ip + 2 > iend) return 0;
        uint32_t offset = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > (uint32_t)(op - dst)) return 0;

        uint32_t matchlen = token & 0xF;
        if (matchlen == 15) {
            uint8_t b;
            do { if (ip >= iend) return 0; b = *ip++; matchlen += b; } while (b == 255);
        }
        matchlen += LZ4_MINMATCH;
        if (matchlen > (uint32_t)(oend - op)) return 0;
        const uint8_t *match = op - offset;
        for (uint32_t i = 0; i < matchlen; i++) *op++ = *match++;   /* overlap-safe */
    }
    return (uint32_t)(op - dst);
}
