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
 *
 * P4 (Amendment 4) adds FILE objects: the frames of one mapped file, keyed by
 * (mount, vfs ident), so every MAP_SHARED mapping of the file shares them. A
 * FILE object has no holders and is never marked, so it lives exactly while
 * something maps it. Write-back (A4.4) copies PTE-dirty pages to the file with
 * the capability each area captured at mmap (D4).
 */
#include "shm.h"
#include "mm.h"
#include "kheap.h"
#include "spinlock.h"
#include "errno.h"
#include "vfs.h"
#include "vmm.h"
#include "tlb.h"
#include "console.h"      /* kline: one write per line (DDR-1055) */

struct shm_obj {
    uint32_t used;                     /* 0 free, 1 live, 2 reclaiming, 3 dead (A2.4) */
    uint32_t npages;
    uint32_t nattch;
    uint32_t holders;
    uint32_t marked;
    uint32_t filling;                  /* P3: frames being allocated, lock dropped */
    uint32_t kind;                     /* SHM_KIND_ANON (SysV, P3) or SHM_KIND_FILE */
    uint64_t fident;                   /* A4.3: FILE key, with file.mnt */
    uint64_t fsize;                    /* A4.3: file size in bytes at first mmap */
    struct vfs_file file;              /* A4.3: copy taken at first mmap */
    uint64_t frames[SHM_MAX_PAGES];
};

#define SHM_KIND_ANON 0
#define SHM_KIND_FILE 1

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
        o->kind = SHM_KIND_ANON;
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
    else if (o->used == 3 && !o->holders)
        o->used = 0;                   /* A2.4: the last shmid naming it is gone */
    spin_unlock_irqrestore(&g_shm_lock, fl);
    if (!go)
        return;
    free_frames(o->frames, o->npages);
    fl = spin_lock_irqsave(&g_shm_lock);
    o->npages = 0;
    /* DDR-1178 A2.4: a removed segment whose shmids still exist keeps its slot
     * dead, or a later segment in the slot would be reached through them. */
    o->used = (o->holders || o->kind == SHM_KIND_FILE) ? 3 : 0;   /* MUTANT S4-7 */
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
    if (g_shm[obj].used) g_shm[obj].holders++;   /* A2.4: dead slots count too */
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
        v->pgoff = 0;
        v->wcap = 0;
    }
}

/* ---- DDR-1178 Amendment 4 (P4): FILE objects ---------------------------- */

static int file_match(const struct shm_obj *o, int mnt, uint64_t ident) {
    return o->used == 1 && o->kind == SHM_KIND_FILE &&
           o->file.mnt == mnt && o->fident == ident;
}

int shm_file_attach(const struct vfs_file *f, uint64_t ident, cap_t rcap) {
    uint64_t size = f->size;
    if (size == 0)
        return -EINVAL;
    uint64_t np = (size + 4095) / 4096;
    if (np > SHM_MAX_PAGES)
        return -EFBIG;

    /* A live, filled object for this file: attach it (A4.3 lookup). */
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    for (int i = 0; i < SHM_MAX; i++) {
        struct shm_obj *o = &g_shm[i];
        if (file_match(o, f->mnt, ident) && !o->filling && o->npages) {
            o->nattch++;
            spin_unlock_irqrestore(&g_shm_lock, fl);
            return i;
        }
    }
    /* None: reserve a slot. `filling` hides it from lookups and from
     * put_and_reclaim until its frames are read. */
    int idx = -1;
    for (int i = 0; i < SHM_MAX; i++)
        if (!g_shm[i].used) { idx = i; break; }
    if (idx >= 0) {
        struct shm_obj *o = &g_shm[idx];
        o->used = 1;
        o->kind = SHM_KIND_FILE;
        o->npages = 0;
        o->nattch = 0;
        o->holders = 0;
        o->marked = 0;
        o->filling = 1;
        o->fident = ident;
        o->fsize = size;
        o->file = *f;
    }
    spin_unlock_irqrestore(&g_shm_lock, fl);
    if (idx < 0)
        return -ENOSPC;

    /* Read the file eagerly, outside the lock (the read blocks). The tail past
     * `size` stays zero: ptnode_alloc zeroes, as DDR-1112's private path. */
    struct shm_obj *o = &g_shm[idx];
    int rc = 0;
    uint32_t got = 0;
    for (; got < (uint32_t)np; got++) {
        void *frame = ptnode_alloc();
        if (!frame) { rc = -ENOMEM; break; }
        o->frames[got] = (uint64_t)(uintptr_t)frame;
        if (vfs_read(rcap, &o->file, (uint64_t)got * 4096, frame, 4096) < 0) {
            got++;                     /* this frame is ours to free too */
            rc = -EIO;
            break;
        }
    }
    if (rc < 0) {
        free_frames(o->frames, got);
        fl = spin_lock_irqsave(&g_shm_lock);
        o->filling = 0;
        o->used = 0;
        spin_unlock_irqrestore(&g_shm_lock, fl);
        return rc;
    }

    /* Publish, unless another first mapper published the same file meanwhile:
     * then attach theirs and drop ours, so one file has one object. */
    fl = spin_lock_irqsave(&g_shm_lock);
    int other = -1;
    for (int i = 0; i < SHM_MAX; i++)
        if (i != idx && file_match(&g_shm[i], f->mnt, ident) &&
            !g_shm[i].filling && g_shm[i].npages) { other = i; break; }
    if (other >= 0) {
        g_shm[other].nattch++;
        o->filling = 0;
        o->used = 2;                   /* reclaiming: no lookup sees it */
    } else {
        o->npages = (uint32_t)np;
        o->nattch = 1;
        o->filling = 0;
    }
    spin_unlock_irqrestore(&g_shm_lock, fl);
    if (other >= 0) {
        free_frames(o->frames, (uint32_t)np);
        fl = spin_lock_irqsave(&g_shm_lock);
        o->used = 0;
        spin_unlock_irqrestore(&g_shm_lock, fl);
        return other;
    }
    return idx;
}

int shm_file_busy(int mnt, uint64_t ident) {
    int busy = 0;
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    for (int i = 0; i < SHM_MAX && !busy; i++)
        if (file_match(&g_shm[i], mnt, ident))
            busy = 1;                  /* a filling object counts: it will map */
    spin_unlock_irqrestore(&g_shm_lock, fl);
    return busy;
}

/* DDR-1178 A4.8: 1 if a FILE object maps (mnt, ident) and `end` lies past
 * its mapped size. Write-back writes through the object's attach-time copy of
 * the vfs_file, so a file grown meanwhile would be cut back to that size. */
int shm_file_grow_busy(int mnt, uint64_t ident, uint64_t end) {
    int refuse = 0;
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    for (int i = 0; i < SHM_MAX; i++)
        if (file_match(&g_shm[i], mnt, ident)) {
            refuse = end > g_shm[i].fsize;
            break;
        }
    spin_unlock_irqrestore(&g_shm_lock, fl);
    return refuse;
}

/* A4.8: the write(2)/io_uring check. Saturates off + len, so a huge offset
 * cannot wrap past the test. */
int shm_vfs_grow_refused(const struct vfs_file *f, uint64_t off, uint64_t len) {
    uint64_t ident;
    if (!shm_file_any(f->mnt) || vfs_ident(f, &ident) != 0)
        return 0;
    uint64_t end = off + len;
    if (end < off)
        end = ~0ull;
    return shm_file_grow_busy(f->mnt, ident, end);
}

int shm_file_any(int mnt) {
    int any = 0;
    uint64_t fl = spin_lock_irqsave(&g_shm_lock);
    for (int i = 0; i < SHM_MAX && !any; i++)
        if (g_shm[i].used == 1 && g_shm[i].kind == SHM_KIND_FILE && g_shm[i].file.mnt == mnt)
            any = 1;
    spin_unlock_irqrestore(&g_shm_lock, fl);
    return any;
}

int shm_obj_is_file(int obj) {
    return obj >= 0 && obj < SHM_MAX && g_shm[obj].kind == SHM_KIND_FILE;
}

/* A4.4. The area holds an attach, so the object, its frames and its file copy
 * stay put while this runs; nothing here takes g_shm_lock, and the writes
 * block on the filesystem (mm_lock -> mnt_lock order, A4.1). */
int shm_vma_writeback(uint64_t cr3, const struct vm_area *v, uint64_t from,
                      uint64_t to, int mode) {
    if (!v->npages || !v->shm || !v->wcap || !cr3)
        return 0;
    int obj = v->shm - 1;
    if (!shm_obj_is_file(obj))
        return 0;
    struct shm_obj *o = &g_shm[obj];
    uint64_t vend = v->base + v->npages * 4096;
    if (from < v->base) from = v->base;
    if (to > vend)      to = vend;
    if (from >= to)
        return 0;
    uint64_t npages = (to - from) / 4096;

    /* Which pages this area dirtied. A page bitmap: an area is at most
     * SHM_MAX_PAGES (256) pages. */
    uint64_t dirty[SHM_MAX_PAGES / 64] = {0};
    uint64_t ndirty = 0;
    for (uint64_t i = 0; i < npages; i++)
        if (vmm_dirty_op(cr3, from + i * 4096, VMM_DIRTY_TEST) == 1) {
            dirty[i / 64] |= 1ull << (i % 64);
            ndirty++;
        }
    if (!ndirty)
        return 0;

    if (mode == SHM_WB_MSYNC) {
        /* msync pays as it goes (A4.2), before anything is cleared, so a
         * refusal loses nothing. Then clear D and drop the TLB entries, so a
         * store after this write-back dirties the page again (A4.4 step 2). */
        if (vfs_write_prepay(ndirty * 4096) < 0)
            return -EAGAIN;
        for (uint64_t i = 0; i < npages; i++)
            if (dirty[i / 64] & (1ull << (i % 64)))
                vmm_dirty_op(cr3, from + i * 4096, VMM_DIRTY_CLEAR);
        tlb_shoot(cr3, from, npages);
    }

    int err = 0;
    for (uint64_t i = 0; i < npages; i++) {
        if (!(dirty[i / 64] & (1ull << (i % 64))))
            continue;
        uint64_t va = from + i * 4096;
        uint64_t pg = v->pgoff + (va - v->base) / 4096;
        uint64_t off = pg * 4096;
        if (off >= o->fsize || pg >= o->npages)
            continue;                  /* the zero tail past EOF is never written */
        uint32_t n = (o->fsize - off < 4096) ? (uint32_t)(o->fsize - off) : 4096u;
        struct vfs_file fc = o->file;  /* the driver updates its copy's fields */
        int r = vfs_write_prepaid(&fc, off,
                                  (const void *)(uintptr_t)o->frames[pg], n);
        if (r != (int)n) {
            err = -EIO;
            if (mode == SHM_WB_MSYNC)
                vmm_dirty_op(cr3, va, VMM_DIRTY_SET);   /* still to be written */
            else {
                kline k; kline_init(&k);
                kline_s(&k, "[shm] writeback failed rc=");
                kline_s(&k, r < 0 ? "-" : "");
                kline_d(&k, (uint64_t)(r < 0 ? -(long)r : (long)r));
                kline_s(&k, "\r\n");
                kline_emit(&k);
            }
        }
    }
    return err;
}

void shm_writeback_vmas(struct mm *mm) {
    for (int i = 0; i < VM_AREA_MAX; i++) {
        struct vm_area *v = &mm->vma[i];
        if (v->npages && v->shm && v->wcap)
            shm_vma_writeback(mm->cr3, v, v->base, v->base + v->npages * 4096,
                              SHM_WB_RELEASE);
    }
}
