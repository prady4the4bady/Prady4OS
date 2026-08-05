/* kernel/aether/agentmem.c — agent memory store (DDR-836).
 *
 * See agentmem.h for why this is a shared blackboard rather than per-agent
 * storage. Fixed-size table in kernel .bss; no allocation on the syscall path,
 * so a memory-pressure failure cannot turn a write into a partial update.
 */
#include "agentmem.h"
#include "string.h"

typedef struct {
    uint8_t  key[MEM_MAX_KEY];      /* NUL-padded; all-zero == free slot */
    uint32_t vallen;
    uint8_t  val[MEM_MAX_VAL];
} mem_rec_t;

static mem_rec_t g_mem[MEM_MAX_RECORDS];

static int key_eq(const uint8_t a[MEM_MAX_KEY], const uint8_t b[MEM_MAX_KEY]) {
    for (unsigned i = 0; i < MEM_MAX_KEY; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int key_is_free(const uint8_t a[MEM_MAX_KEY]) {
    for (unsigned i = 0; i < MEM_MAX_KEY; i++)
        if (a[i])
            return 0;
    return 1;
}

/* Pack into the fixed field, NUL-padded. A key that does not fit is REJECTED
 * rather than truncated: two distinct keys truncating to the same bytes would
 * silently alias two different facts into one record. */
static int key_pack(uint8_t out[MEM_MAX_KEY], const char *key) {
    unsigned i = 0;
    if (!key || !key[0])
        return -1;
    for (; key[i]; i++) {
        if (i >= MEM_MAX_KEY)
            return -1;
        out[i] = (uint8_t)key[i];
    }
    for (; i < MEM_MAX_KEY; i++)
        out[i] = 0;
    return 0;
}

int agentmem_write(const char *key, const void *val, uint32_t vallen) {
    if (!val || vallen == 0 || vallen > MEM_MAX_VAL)
        return MEM_ERR_ARG;

    uint8_t k[MEM_MAX_KEY];
    if (key_pack(k, key) != 0)
        return MEM_ERR_ARG;

    mem_rec_t *slot = 0;
    for (unsigned i = 0; i < MEM_MAX_RECORDS; i++) {
        if (!key_is_free(g_mem[i].key) && key_eq(g_mem[i].key, k)) {
            slot = &g_mem[i];
            break;
        }
    }
    if (!slot) {
        for (unsigned i = 0; i < MEM_MAX_RECORDS; i++) {
            if (key_is_free(g_mem[i].key)) { slot = &g_mem[i]; break; }
        }
    }
    if (!slot)
        return MEM_ERR_NOSPC;                /* reject, never evict — DDR-836 */

    memcpy(slot->key, k, MEM_MAX_KEY);
    memcpy(slot->val, val, vallen);
    slot->vallen = vallen;
    return MEM_OK;
}

int agentmem_read(const char *key, void *out, uint32_t *outlen) {
    if (!out || !outlen)
        return MEM_ERR_ARG;

    uint8_t k[MEM_MAX_KEY];
    if (key_pack(k, key) != 0)
        return MEM_ERR_ARG;

    for (unsigned i = 0; i < MEM_MAX_RECORDS; i++) {
        if (key_is_free(g_mem[i].key) || !key_eq(g_mem[i].key, k))
            continue;
        memcpy(out, g_mem[i].val, g_mem[i].vallen);
        *outlen = g_mem[i].vallen;
        return MEM_OK;
    }
    return MEM_ERR_NOENT;
}
