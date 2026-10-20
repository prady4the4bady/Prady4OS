/* kernel/cap.c — capability tables with generation-counter revocation (2c). */
#include "cap.h"
#include "kheap.h"
#include "spinlock.h"     /* DDR-1181 §3.6: one table, several threads */
#include "shm.h"          /* DDR-1178 A2.2: a RES_SHM slot holds its segment */

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
    uint32_t        refs;    /* DDR-1181 §3.6: tcbs sharing this table (atomic) */
    spinlock_t      lk;      /* DDR-1181 §3.6: guards every slot. Threads share
                              * one table, so a revoke on one CPU and a validate
                              * on another must not interleave inside a slot. */
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
    t->lk.v = 0;                       /* kmalloc does not zero */
    t->refs = 1;
    return t;
}

void cap_table_get(struct cap_table *t) {
    __atomic_add_fetch(&t->refs, 1, __ATOMIC_ACQ_REL);
}

/* DDR-1181 §3.6: drops one reference; the last one frees the table. */
void cap_table_destroy(struct cap_table *t) {
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    /* DDR-1178 A2.2: the last reference to this table releases every segment
     * hold it carried. No lock: nobody else can reach the table now. */
    for (int i = 0; i < CAP_SLOTS; i++)
        if (t->slots[i].in_use && t->slots[i].res_type == RES_SHM)
            shm_unhold((int)t->slots[i].res_id);
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

/* Caller holds t->lk. */
static cap_t create_locked(struct cap_table *t, uint32_t res_type, uint64_t res_id, uint32_t rights) {
    for (uint32_t i = 0; i < CAP_SLOTS; i++) {
        struct cap_slot *s = &t->slots[i];
        if (!s->in_use) {
            s->generation++;          /* unique, >= 1, per (re)use */
            s->in_use = 1;
            s->rights = rights;
            s->res_type = res_type;
            s->res_id = res_id;
            if (res_type == RES_SHM)
                shm_hold((int)res_id);   /* DDR-1178 A2.2 */
            return make_handle(i, s->generation);
        }
    }
    return CAP_NULL;                   /* table full */
}

cap_t cap_create(struct cap_table *t, uint32_t res_type, uint64_t res_id, uint32_t rights) {
    if (!t)
        return CAP_NULL;
    uint64_t fl = spin_lock_irqsave(&t->lk);
    cap_t h = create_locked(t, res_type, res_id, rights);
    spin_unlock_irqrestore(&t->lk, fl);
    return h;
}

cap_t cap_restrict(struct cap_table *t, cap_t h, uint32_t keep_rights) {
    if (!t)
        return CAP_NULL;
    uint64_t fl = spin_lock_irqsave(&t->lk);
    struct cap_slot *s = resolve(t, h);
    cap_t r = s ? create_locked(t, s->res_type, s->res_id, s->rights & keep_rights) : CAP_NULL;
    spin_unlock_irqrestore(&t->lk, fl);
    return r;
}

cap_t cap_delegate(struct cap_table *src, cap_t h, struct cap_table *dst, uint32_t subset_rights) {
    if (!src || !dst)
        return CAP_NULL;
    /* Snapshot the source slot under ITS lock, then mint under dst's: never
     * both at once, so two delegations in opposite directions cannot deadlock. */
    uint64_t fl = spin_lock_irqsave(&src->lk);
    struct cap_slot *s = resolve(src, h);
    struct cap_slot snap;
    if (s)
        snap = *s;
    spin_unlock_irqrestore(&src->lk, fl);
    if (!s)
        return CAP_NULL;
    return cap_create(dst, snap.res_type, snap.res_id, snap.rights & subset_rights);
}

int cap_fork(const struct cap_table *src, struct cap_table *dst) {
    if (!src || !dst)
        return -1;
    /* src is the parent's live table (shared by its threads); dst is the new
     * child's, not yet visible to anyone, so only src needs its lock. */
    struct cap_table *ws = (struct cap_table *)src;
    uint64_t fl = spin_lock_irqsave(&ws->lk);
    for (int i = 0; i < CAP_SLOTS; i++)   /* exact copy: handles stay valid in dst */
        dst->slots[i] = src->slots[i];
    spin_unlock_irqrestore(&ws->lk, fl);
    for (int i = 0; i < CAP_SLOTS; i++)   /* DDR-1178 A2.2: the copies hold too */
        if (dst->slots[i].in_use && dst->slots[i].res_type == RES_SHM)
            shm_hold((int)dst->slots[i].res_id);
    return 0;
}

int cap_revoke(struct cap_table *t, cap_t h) {
    if (!t)
        return -1;
    uint64_t fl = spin_lock_irqsave(&t->lk);
    struct cap_slot *s = resolve(t, h);
    int shm_obj = -1;
    if (s) {
        if (s->res_type == RES_SHM)
            shm_obj = (int)s->res_id;
        s->generation++;              /* outstanding handles no longer match */
        s->in_use = 0;
    }
    spin_unlock_irqrestore(&t->lk, fl);
    if (shm_obj >= 0)
        shm_unhold(shm_obj);          /* DDR-1178 A2.2, outside the table lock */
    return s ? 0 : -1;
}

uint32_t cap_validate(struct cap_table *t, cap_t h, uint32_t required) {
    if (!t)
        return 0;
    uint64_t fl = spin_lock_irqsave(&t->lk);
    struct cap_slot *s = resolve(t, h);
    uint32_t ok = s && (s->rights & required) == required;
    spin_unlock_irqrestore(&t->lk, fl);
    return ok;
}

uint32_t cap_authorize(struct cap_table *t, cap_t h, uint32_t res_type,
                       uint64_t res_id, uint32_t required) {
    if (!t)
        return 0;
    uint64_t fl = spin_lock_irqsave(&t->lk);
    struct cap_slot *s = resolve(t, h);
    uint32_t ok = s && s->res_type == res_type && s->res_id == res_id &&
                  (s->rights & required) == required;
    spin_unlock_irqrestore(&t->lk, fl);
    return ok;
}

uint32_t cap_lookup(struct cap_table *t, cap_t h, uint32_t res_type,
                    uint64_t *res_id, uint32_t *rights) {
    if (!t)
        return 0;
    uint64_t fl = spin_lock_irqsave(&t->lk);
    struct cap_slot *s = resolve(t, h);
    uint32_t ok = s && s->res_type == res_type;
    if (ok) {
        *res_id = s->res_id;
        *rights = s->rights;
    }
    spin_unlock_irqrestore(&t->lk, fl);
    return ok;
}
