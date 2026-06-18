/* kernel/drivers/blk/blk.c — block device registry + read/write dispatch. */
#include "blk.h"

#define BLK_MAX 8

static struct blk_device *g_blk[BLK_MAX];
static unsigned g_n;

void blk_register(struct blk_device *bd) {
    if (g_n < BLK_MAX)
        g_blk[g_n++] = bd;
}

unsigned blk_count(void) { return g_n; }

struct blk_device *blk_get(unsigned i) {
    return (i < g_n) ? g_blk[i] : 0;
}

int blk_read(unsigned dev, uint64_t lba, void *buf, uint32_t count) {
    if (dev >= g_n || !g_blk[dev]->read)
        return -1;
    return g_blk[dev]->read(g_blk[dev], lba, buf, count);
}

int blk_write(unsigned dev, uint64_t lba, const void *buf, uint32_t count) {
    if (dev >= g_n || !g_blk[dev]->write)
        return -1;
    return g_blk[dev]->write(g_blk[dev], lba, buf, count);
}
