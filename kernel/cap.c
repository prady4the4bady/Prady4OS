/* kernel/cap.c — capability tables with generation-counter revocation (2c). */
#include "cap.h"
#include "kheap.h"

#define CAP_SLOTS 64

struct cap_slot {
    uint32_t in_use;
    uint32_t generation;     /* bumped on (re)use and on revoke; 0 == never valid */
    uint32_t rights;
    uint32_t res_type;
    uint64_t res_id;
};

struct cap_table {
    struct cap_slot slots[CAP_SLOTS];
};

static uint32_t h_index(cap_t h) { return (uint32_t)(h & 0xFFFFFFFFu); }
static uint32_t h_gen(cap_t h)   { return (uint32_t)(h >> 32); }
static cap_t    make_handle(uint32_t idx, uint32_t gen) { return ((cap_t)gen << 32) | idx; }

struct cap_table *cap_table_create(void) {
    struct cap_table *t = (struct cap_table *)kmalloc(sizeof(struct cap_table));
    if (!t)
        return 0;
    for (int i = 0; i < CAP_SLOTS; i++) {
        t->slots[i].in_use = 0;
        t->slots[i].generation = 0;
        t->slots[i].rights = 0;
        t->slots[i].res_type = 0;
        t->slots[i].res_id = 0;
    }
    return t;
}

void cap_table_destroy(struct cap_table *t) {
    kfree(t);
}

/* Resolve a handle to its slot, or NULL if invalid/stale. */
static struct cap_slot *resolve(struct cap_table *t, cap_t h) {
    if (!t)
        return 0;
    uint32_t idx = h_index(h);
    if (idx >= CAP_SLOTS)
        return 0;
    struct cap_slot *s = &t->slots[idx];
    if (!s->in_use)
        return 0;
    if (s->generation != h_gen(h))   /* stale (revoked / reused) */
        return 0;
    return s;
}

cap_t cap_create(struct cap_table *t, uint32_t res_type, uint64_t res_id, uint32_t rights) {
    if (!t)
        return CAP_NULL;
    for (uint32_t i = 0; i < CAP_SLOTS; i++) {
        struct cap_slot *s = &t->slots[i];
        if (!s->in_use) {
            s->generation++;          /* unique, >= 1, per (re)use */
            s->in_use = 1;
            s->rights = rights;
            s->res_type = res_type;
            s->res_id = res_id;
            return make_handle(i, s->generation);
        }
    }
    return CAP_NULL;                   /* table full */
}

cap_t cap_restrict(struct cap_table *t, cap_t h, uint32_t keep_rights) {
    struct cap_slot *s = resolve(t, h);
    if (!s)
        return CAP_NULL;
    return cap_create(t, s->res_type, s->res_id, s->rights & keep_rights);
}

cap_t cap_delegate(struct cap_table *src, cap_t h, struct cap_table *dst, uint32_t subset_rights) {
    struct cap_slot *s = resolve(src, h);
    if (!s)
        return CAP_NULL;
    return cap_create(dst, s->res_type, s->res_id, s->rights & subset_rights);
}

int cap_revoke(struct cap_table *t, cap_t h) {
    struct cap_slot *s = resolve(t, h);
    if (!s)
        return -1;
    s->generation++;                  /* outstanding handles no longer match */
    s->in_use = 0;
    return 0;
}

uint32_t cap_validate(struct cap_table *t, cap_t h, uint32_t required) {
    struct cap_slot *s = resolve(t, h);
    if (!s)
        return 0;
    if ((s->rights & required) != required)
        return 0;
    return 1;
}
