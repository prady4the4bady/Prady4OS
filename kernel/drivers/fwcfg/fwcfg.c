/* kernel/drivers/fwcfg/fwcfg.c — DDR-804: per-boot probe selection via QEMU fw_cfg.
 *
 * Register and directory layout follow QEMU's documented fw_cfg interface. They
 * are verified empirically by the first consumer's gate rather than trusted: if
 * any of it were wrong the probe would simply never be selected and that gate
 * would fail on a missing sentinel. There is no path here that can silently
 * report success on a bad read.
 *
 * Everything the device reports is treated as untrusted input and clamped
 * before use — a corrupt count must not drive a loop (S2).
 */
#include "fwcfg.h"
#include "../../io.h"
#include <stdint.h>

#define FWCFG_PORT_SEL    0x510      /* 16-bit write: select a key */
#define FWCFG_PORT_DATA   0x511      /* 8-bit read: sequential from offset 0 */

#define FWCFG_KEY_SIGNATURE  0x0000  /* -> "QEMU" */
#define FWCFG_KEY_FILE_DIR   0x0019  /* -> BE u32 count, then entries */

#define FWCFG_MAX_ENTRIES    64u     /* directory scan bound (S2) */
#define FWCFG_PROBES_MAX     256u    /* probe-list byte bound (S2) */
#define FWCFG_NAME_LEN       56u     /* fixed field; NOT assumed NUL-terminated */

#define PROBE_FILE_NAME "opt/org.pradyos/probes"

static char     g_probes[FWCFG_PROBES_MAX + 1];   /* always NUL-terminated */
static unsigned g_probes_len;

static void fwcfg_select(uint16_t key) { outw(FWCFG_PORT_SEL, key); }

static void fwcfg_read(void *buf, uint32_t len) {
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++)
        p[i] = inb(FWCFG_PORT_DATA);
}

/* The directory is big-endian; the host is not. Assemble explicitly rather than
 * byte-swapping in place, so the intent survives a reader who forgets. */
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Length-bounded compare against the fixed 56-byte name field. The field is
 * padded, not necessarily terminated, so this stops at the shorter of the two
 * and requires the field byte just past the match to be NUL or padding. */
static int name_matches(const uint8_t *field, const char *want) {
    unsigned i = 0;
    for (; i < FWCFG_NAME_LEN && want[i]; i++)
        if (field[i] != (uint8_t)want[i])
            return 0;
    if (want[i])                       /* wanted name longer than the field */
        return 0;
    return i == FWCFG_NAME_LEN || field[i] == 0;
}

void fwcfg_init(void) {
    g_probes[0] = 0;
    g_probes_len = 0;

    /* Fail closed on anything that is not a fw_cfg device answering as QEMU. */
    uint8_t sig[4];
    fwcfg_select(FWCFG_KEY_SIGNATURE);
    fwcfg_read(sig, sizeof sig);
    if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U')
        return;

    fwcfg_select(FWCFG_KEY_FILE_DIR);
    uint8_t cnt_be[4];
    fwcfg_read(cnt_be, sizeof cnt_be);
    uint32_t count = be32(cnt_be);
    if (count > FWCFG_MAX_ENTRIES)      /* clamp: never trust the device's count */
        count = FWCFG_MAX_ENTRIES;

    /* Entries stream sequentially from the same selected key, so the whole
     * directory must be walked in order — a matching entry cannot be seeked to. */
    uint16_t sel = 0;
    uint32_t size = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t ent[4 + 2 + 2 + FWCFG_NAME_LEN];
        fwcfg_read(ent, sizeof ent);
        if (!sel && name_matches(ent + 8, PROBE_FILE_NAME)) {
            size = be32(ent);
            sel  = be16(ent + 4);
            /* Keep reading: the stream must be drained in order, and stopping
             * early would leave the device mid-directory for any later reader. */
        }
    }
    if (!sel)
        return;                          /* file absent: no probes selected */

    if (size > FWCFG_PROBES_MAX)         /* clamp before the read, not after */
        size = FWCFG_PROBES_MAX;
    fwcfg_select(sel);
    fwcfg_read(g_probes, size);
    g_probes[size] = 0;
    g_probes_len = (unsigned)size;
}

int probe_enabled(const char *name) {
    if (!g_probes_len || !name || !name[0])
        return 0;
    /* Whole-field match against a comma-separated list, so "net" does not match
     * "privnet" and a trailing name is not missed. */
    unsigned i = 0;
    while (i < g_probes_len) {
        unsigned start = i;
        while (i < g_probes_len && g_probes[i] != ',')
            i++;
        unsigned len = i - start;
        unsigned k = 0;
        while (k < len && name[k] && g_probes[start + k] == name[k])
            k++;
        if (k == len && !name[k])
            return 1;
        if (i < g_probes_len)
            i++;                         /* step over the comma */
    }
    return 0;
}
