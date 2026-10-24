/* kernel/ipc/shm.c — SysV shared-memory segment objects (DDR-1178 Amendment 2).
 *
 * A segment is a fixed list of frames. The object holds one reference to each
 * frame; every mapping of it adds one (pmm_incref) and drops it on unmap. The
 * object goes away when NOTHING can reach it any more and nothing maps it:
 *
 *     nattch == 0  &&  (marked || holders == 0)
 *
 * `marked` is IPC_RMID (D2). `holders` counts capability-table slots naming the
 * object (A2.1: the shmid IS such a capability), so a segment whose last holder
 * exited without IPC_RMID is reclaimed instead of pinning a slot forever -- the
 * same rule as a file whose last descriptor closed.
 *
 * P3 (Amendment 3) reuses the table for shm_open(SHM_ANON): an FD_SHM fd entry
 * is a holder, the object starts EMPTY (npages 0) and ftruncate sizes it once.
 * A POSIX object is never marked; it goes at nattch == 0 && holders == 0.
 */
#include "shm.h"
#include "mm.h"
#include "kheap.h"
#include "spinlock.h"
#include "errno.h"

struct shm_obj {
    uint32_t used;                     /* 0 free, 1 live, 2 reclaiming */
    uint32_t npages;
    uint32_t nattch;
    uint32_t holders;
    uint32_t marked;
    uint32_t filling;                  /* P3: frames being allocated, lock dropped */
    uint64_t frames[SHM_MAX_PAGES];
};

static struct shm_obj g_shm[SHM_MAX];
static spinlock_t g_shm_lock = SPINLOCK_INIT;

/* Drop the object's own frame references, OUTSIDE the lock. A frame still
 * mapped somewhere keeps its other references; this one is the object's. */
static void free_frames(uint64_t *f, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        ptnode_free((void *)(uintptr_t)f[i]);
}

int shm_obj_reserve(void) {
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    int idx = -1;
    for (int i = 0; i < SHM_MAX; i++)
        if (!g_shm[i].used) { idx = i; break; }
    if (idx >= 0) {
        struct shm_obj *o = &g_shm[idx];
        o->used = 1;
        o->npages = 0;                 /* empty until shm_obj_size */
        o->nattch = 0;
        o->holders = 1;                /* the caller's fd or capability */
        o->marked = 0;
        o->filling = 0;
    }
    spin_unlock_irqrestore(&g_shm_lock, fl);
    return idx < 0 ? -ENOSPC : idx;
}

int shm_obj_size(int obj, uint32_t npages) {
    if (obj < 0 || obj >= SHM_MAX || npages == 0 || npages > SHM_MAX_PAGES)
        return -EINVAL;
    struct shm_obj *o = &g_shm[obj];
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    int ok = o->used == 1 && !o->npages && !o->filling;
    if (ok)
        o->filling = 1;                /* a second ftruncate is refused, and
                                        * put_and_reclaim leaves the slot alone */
    spin_unlock_irqrestore(&g_shm_lock, fl);
    if (!ok)
        return -EINVAL;                /* P3: sized exactly once */
    for (uint32_t i = 0; i < npages; i++) {
        void *f = ptnode_alloc();      /* zeroed */
        if (!f) {
            free_frames(o->frames, i);
            fl = spin_lock_irqsave(&g_shm_lock);
            o->filling = 0;            /* stays empty; its holder reclaims it */
            spin_unlock_irqrestore(&g_shm_lock, fl);
            return -ENOMEM;
        }
        o->frames[i] = (uint64_t)(uintptr_t)f;
    }
    fl = spin_lock_irqsave(&g_shm_lock);
    o->npages = npages;
    o->filling = 0;
    spin_unlock_irqrestore(&g_shm_lock, fl);
    return 0;
}

int shm_obj_create(uint32_t npages) {
    if (npages == 0 || npages > SHM_MAX_PAGES)
        return -EINVAL;
    int idx = shm_obj_reserve();
    if (idx < 0)
        return idx;
    int rc = shm_obj_size(idx, npages);
    if (rc < 0) {
        shm_unhold(idx);               /* the reservation's hold: reclaims it */
        return rc;
    }
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    g_shm[idx].holders = 0;            /* the minted capability takes it over */
    spin_unlock_irqrestore(&g_shm_lock, fl);
    return idx;
}

uint64_t shm_obj_frame(int obj, uint32_t i) {
    if (obj < 0 || obj >= SHM_MAX || i >= g_shm[obj].npages)
        return 0;
    return g_shm[obj].frames[i];
}

uint32_t shm_obj_npages(int obj) {
    return (obj < 0 || obj >= SHM_MAX) ? 0 : g_shm[obj].npages;
}

int shm_attach_begin(int obj) {
    if (obj < 0 || obj >= SHM_MAX)
        return -EINVAL;
    int rc = 0;
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    struct shm_obj *o = &g_shm[obj];
    if (o->used != 1 || !o->npages) rc = -EINVAL;
    else if (o->marked)         rc = -EIDRM;      /* D2 */
    else                        o->nattch++;
    spin_unlock_irqrestore(&g_shm_lock, fl);
    return rc;
}

/* The common tail of every "one fewer of X" path. When the object becomes
 * unreachable and unmapped it enters state 2 (reclaiming): no create reuses the
 * slot and no attach or hold sees it, so its frame list stays intact while the
 * frames are freed OUTSIDE the lock; only then is the slot released. */
#define PUT_HOLDER  0
#define PUT_ATTACH  1
#define PUT_NONE    2      /* re-evaluate only (IPC_RMID): no count drops */
static void put_and_reclaim(int obj, int which) {
    struct shm_obj *o = &g_shm[obj];
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    if (which == PUT_ATTACH)      { if (o->nattch)  o->nattch--; }
    else if (which == PUT_HOLDER) { if (o->holders) o->holders--; }
    /* P3: an object never sized (npages 0) is reclaimed the same way; one
     * being sized is not, its sizer holds it until shm_obj_size returns. */
    int go = o->used == 1 && !o->filling && !o->nattch && (o->marked || !o->holders);
    if (go)
        o->used = 2;
    spin_unlock_irqrestore(&g_shm_lock, fl);
    if (!go)
        return;
    free_frames(o->frames, o->npages);
    fl = spin_lock_irqsave(&g_shm_lock);
    o->npages = 0;
    o->used = 0;
    spin_unlock_irqrestore(&g_shm_lock, fl);
}

void shm_detach(int obj) {
    if (obj >= 0 && obj < SHM_MAX)
        put_and_reclaim(obj, PUT_ATTACH);
}

int shm_mark(int obj) {
    if (obj < 0 || obj >= SHM_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    int ok = g_shm[obj].used == 1 && g_shm[obj].npages;
    if (ok) g_shm[obj].marked = 1;
    spin_unlock_irqrestore(&g_shm_lock, fl);
    if (!ok)
        return -EINVAL;
    /* A marked, unmapped segment goes now; a mapped one at its last detach.
     * PUT_NONE: IPC_RMID is not a detach. Passing an attach drop here took
     * nattch 1 -> 0 on a segment still mapped, reclaimed it under the mapping
     * and freed the slot for reuse, so the next shmat saw -EINVAL instead of
     * -EIDRM (smoke-shm arm J, eidrm=-22, CI runs 39516558534/39518678592). */
    put_and_reclaim(obj, PUT_NONE);
    return 0;
}

void shm_hold(int obj) {
    if (obj < 0 || obj >= SHM_MAX)
        return;
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    if (g_shm[obj].used == 1) g_shm[obj].holders++;
    spin_unlock_irqrestore(&g_shm_lock, fl);
}

void shm_unhold(int obj) {
    if (obj >= 0 && obj < SHM_MAX)
        put_and_reclaim(obj, PUT_HOLDER);
}

void shm_fork_vmas(struct mm *child) {
    for (int i = 0; i < VM_AREA_MAX; i++) {
        struct vm_area *v = &child->vma[i];
        if (!v->npages || !v->shm)
            continue;
        uint64_t fl = spin_lock_irqsave(&g_shm_lock);
        g_shm[v->shm - 1].nattch++;   /* the parent's attach keeps it alive */
        spin_unlock_irqrestore(&g_shm_lock, fl);
    }
}

void shm_release_vmas(struct mm *mm) {
    for (int i = 0; i < VM_AREA_MAX; i++) {
        struct vm_area *v = &mm->vma[i];
        if (v->npages && v->shm)
            shm_detach(v->shm - 1);
        v->shm = 0;
        v->shm_ro = 0;
    }
}
