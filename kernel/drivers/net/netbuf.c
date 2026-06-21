/* kernel/drivers/net/netbuf.c — packet-buffer pool (NET-A). */
#include "netbuf.h"
#include "pmm.h"

#define NETBUF_COUNT 40              /* RX ring (16) + TX + slack */

static uint64_t pool[NETBUF_COUNT];  /* LIFO free stack of buffer addresses */
static int      top;                 /* number of free buffers */
static int      inited;

void netbuf_init(void) {
    if (inited)
        return;
    inited = 1;
    top = 0;
    for (int i = 0; i < NETBUF_COUNT; i++) {
        uint64_t p = pmm_alloc_page();   /* page-aligned, contiguous, identity-mapped */
        if (!p)
            break;
        pool[top++] = p;
    }
}

uint64_t netbuf_alloc(void) {
    return (top > 0) ? pool[--top] : 0;
}

void netbuf_free(uint64_t buf) {
    if (buf && top < NETBUF_COUNT)
        pool[top++] = buf;
}
