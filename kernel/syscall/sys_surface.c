/* kernel/syscall/sys_surface.c — per-client surfaces (Layer 7, DDR-706).
 *
 * A surface is a kernel-owned, physically-contiguous PMM buffer (BGRA) that the
 * kernel maps into both the client (to draw) and the compositor (to read) — the
 * same shared-physical model as SYS_FB_MAP, so the compositor composites straight
 * from the client's pixels with no copy. Bounded table of 16 surfaces.
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "vmm.h"
#include "pmm.h"
#include "spinlock.h"

#define SURFACE_MAX        16
#define SURFACE_DIM_MAX    512u                /* per-surface buffer <= 1 MiB (512*512*4) */
#define SURFACE_VA_BASE    0x8600000000ull     /* below the FB mapping (0x8700000000) */
#define SURFACE_VA_SLOT    0x100000ull         /* 1 MiB per surface id */

/* DDR-729: a client/compositor mapping is a VIEW of surface-layer-owned frames.
 * PTE_SW_SHARED makes free_subtree (vmm_destroy_address_space) skip these leaves
 * exactly like the vDSO, so address-space teardown never frees surface frames —
 * the surface layer is the sole owner and surface_free_slot the sole free point. */
#define SURF_VIEW_FLAGS    (VMM_USER | VMM_RW | VMM_NX | PTE_SW_SHARED)

#define SURFACE_KQ 32                          /* per-surface key ring (DDR-708) */
#define SURFACE_EQ 8                           /* per-surface event ring (DDR-718) */
/* Event types, ALL of them — this comment used to name only type 1, which is
 * how a duplicate number gets shipped (DDR-998 §2):
 *   1 = RESIZE_REQ(w,h)   DDR-718
 *   2 = SCROLL(delta)     DDR-725
 *   3 = COMPOSITED        DDR-911
 *   4 = CLOSE             DDR-998 — "save your state, you are about to go" */
struct surf_event { uint16_t type, arg0, arg1; };
struct surface {
    uint64_t phys;
    uint32_t w, h, npages;
    uint32_t owner_pid;
    int32_t  x, y, z;                           /* z = stacking order (DDR-708) */
    uint8_t  used, committed, focused;
    uint8_t  kq[SURFACE_KQ];                    /* forwarded keystrokes (focused window) */
    uint8_t  kq_head, kq_tail;
    char     title[16];                         /* window title, NUL-terminated (DDR-715) */
    struct surf_event eq[SURFACE_EQ];           /* compositor->owner events (DDR-718) */
    uint8_t  eq_head, eq_tail;
    /* DDR-998: bumped on every allocation of this slot. An id does NOT identify
     * a surface — 16 slots are recycled immediately by surf_take_free, so a
     * deferred action holding only an id can land on a different process's
     * window that merely inherited the number. Holders pair (id, gen). */
    uint32_t gen;
};
static struct surface g_surf[SURFACE_MAX];
static int32_t g_z_top;                         /* monotonic stacking counter */
static spinlock_t g_surf_lock = SPINLOCK_INIT;  /* DDR-729: guards g_surf slot lifecycle */

/* DDR-998: `gen` APPENDED. This struct is declared TWICE — here and at
 * user/compositor.c:43 — and sys_surface_poll copies out
 * count * sizeof(struct surface_info) into the caller's array, so a size
 * disagreement silently overruns the compositor's stack buffer. Both
 * declarations must change in the SAME commit (§INV.13's PT_HI lesson). */
struct surface_info { uint32_t id, w, h; int32_t x, y, z; uint32_t focused; char title[16];
                      uint32_t gen; };

static unsigned order_for(uint64_t npages) {
    unsigned o = 0;
    while (((uint64_t)1 << o) < npages && o < 10) o++;
    return o;
}

/* DDR-729 — the ONE free point. Under g_surf_lock, capture the slot's frames and
 * clear it (used=0); the caller returns the frames to the buddy allocator OUTSIDE
 * the lock (pmm_free is not held under IRQ-off). Returns 0 if the slot was live
 * (caller must free *phys at *order), 1 if it was already empty (nothing to do). */
static int surf_take_free(struct surface *s, uint64_t *phys, unsigned *order) {
    if (!s->used)
        return 1;
    *phys  = s->phys;
    *order = order_for(s->npages);
    /* DDR-998: this wipe is whole-struct, so it would take `gen` with it and
     * every tenancy would come back as gen 1 — a generation counter that
     * counts to one is not a generation counter. Save and restore it across
     * the clear. Found by reading the FREE path after writing the bump in the
     * ALLOC path; the bump alone looks correct in isolation. */
    uint32_t keep_gen = s->gen;
    for (unsigned i = 0; i < sizeof *s; i++) ((uint8_t *)s)[i] = 0;   /* clears used */
    s->gen = keep_gen;
    return 0;
}

static long sys_surface_create(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    uint32_t w = (uint32_t)a1, h = (uint32_t)a2;
    if (w == 0 || h == 0 || w > SURFACE_DIM_MAX || h > SURFACE_DIM_MAX)
        return -EINVAL;
    /* Allocate + zero the buffer BEFORE the lock (a memset of up to 1 MiB must
     * not run under the IRQ-off slot lock). */
    uint64_t bytes = (uint64_t)w * h * 4;
    uint64_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(order_for(npages));
    if (!phys)
        return -ENOMEM;
    for (uint64_t i = 0; i < npages * PAGE_SIZE; i++)   /* zero (identity-mapped) */
        ((volatile uint8_t *)(uintptr_t)phys)[i] = 0;

    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    int id = -1;
    for (int i = 0; i < SURFACE_MAX; i++) if (!g_surf[i].used) { id = i; break; }
    if (id < 0) {
        spin_unlock_irqrestore(&g_surf_lock, fl);
        pmm_free_pages(phys, order_for(npages));       /* table full: return frames */
        return -EMFILE;
    }
    struct surface *s = &g_surf[id];
    /* DDR-998: bump BEFORE anything else in this slot becomes visible. `gen` is
     * the only field deliberately NOT reset here — it must survive the slot's
     * reuse, since its whole job is to tell one tenancy from the next. */
    s->gen++;
    s->phys = phys; s->w = w; s->h = h; s->npages = (uint32_t)npages;
    s->owner_pid = current_thread->pid; s->x = s->y = 0;
    s->z = ++g_z_top; s->focused = 0;
    s->kq_head = s->kq_tail = 0;
    s->eq_head = s->eq_tail = 0;                /* empty event ring (DDR-718) */
    s->title[0] = 0;                            /* untitled until SET_TITLE (DDR-715) */
    s->used = 1; s->committed = 0;
    spin_unlock_irqrestore(&g_surf_lock, fl);
    return id;
}

static long sys_surface_map(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    if (!s->used) { spin_unlock_irqrestore(&g_surf_lock, fl); return -EINVAL; }
    if (s->owner_pid != current_thread->pid) {  /* a client maps only its own surface */
        spin_unlock_irqrestore(&g_surf_lock, fl);
        return -EPERM;
    }
    uint64_t phys = s->phys; uint32_t npages = s->npages;
    spin_unlock_irqrestore(&g_surf_lock, fl);   /* map on locals, not under the lock */
    uint64_t va = SURFACE_VA_BASE + (uint64_t)id * SURFACE_VA_SLOT;
    for (uint32_t i = 0; i < npages; i++)
        if (vmm_map_in(current_thread->cr3, va + (uint64_t)i * PAGE_SIZE,
                       phys + (uint64_t)i * PAGE_SIZE, SURF_VIEW_FLAGS) != 0)
            return -ENOMEM;
    return (long)va;
}

/* The compositor maps a client's surface to read it; owner check is skipped (the
 * compositor reads pixels it will composite, never gaining the owner's authority).
 * Distinguished from sys_surface_map by being the compositor path — same VA. */
static long sys_surface_map_ro(int id) {
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    if (!s->used) { spin_unlock_irqrestore(&g_surf_lock, fl); return -EINVAL; }
    uint64_t phys = s->phys; uint32_t npages = s->npages;
    spin_unlock_irqrestore(&g_surf_lock, fl);
    uint64_t va = SURFACE_VA_BASE + (uint64_t)id * SURFACE_VA_SLOT;
    for (uint32_t i = 0; i < npages; i++)
        if (vmm_map_in(current_thread->cr3, va + (uint64_t)i * PAGE_SIZE,
                       phys + (uint64_t)i * PAGE_SIZE, SURF_VIEW_FLAGS) != 0)
            return -ENOMEM;
    return (long)va;
}

/* x == SURF_POS_KEEP (INT32_MAX) keeps the current position (DDR-719): the
 * compositor owns placement (drag/maximize moves), so a client re-commit after
 * an event-channel resize must not stomp it. */
#define SURF_POS_KEEP 0x7FFFFFFF
static long sys_surface_commit(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    long r = 0;
    if (!s->used)                                  r = -EINVAL;
    else if (s->owner_pid != current_thread->pid)  r = -EPERM;
    else {
        if ((int32_t)a2 != SURF_POS_KEEP) {
            s->x = (int32_t)a2;
            s->y = (int32_t)a3;
        }
        s->committed = 1;
    }
    spin_unlock_irqrestore(&g_surf_lock, fl);
    return r;
}

/* (id<0) -> POLL: list committed surfaces; (id>=0) -> compositor read-map.
 * One handler multiplexes so the compositor can map a surface it just polled.
 * a1 = buf/-(id+1)... kept as two distinct entry points below for clarity. */
static long sys_surface_poll(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    int max = (int)a2;
    if (max <= 0) return 0;
    if (max > SURFACE_MAX) max = SURFACE_MAX;
    struct surface_info buf[SURFACE_MAX];
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);   /* consistent snapshot */
    for (int i = 0; i < SURFACE_MAX && n < max; i++) {
        if (g_surf[i].used && g_surf[i].committed) {
            buf[n].id = (uint32_t)i; buf[n].w = g_surf[i].w; buf[n].h = g_surf[i].h;
            buf[n].x = g_surf[i].x;  buf[n].y = g_surf[i].y;
            buf[n].z = g_surf[i].z;  buf[n].focused = g_surf[i].focused;
            buf[n].gen = g_surf[i].gen;                      /* DDR-998 */
            for (int c = 0; c < 16; c++) buf[n].title[c] = g_surf[i].title[c];
            n++;
        }
    }
    spin_unlock_irqrestore(&g_surf_lock, fl);        /* copyout never under the lock */
    for (int i = 1; i < n; i++) {                /* insertion sort by z ascending */
        struct surface_info t = buf[i];
        int j = i - 1;
        while (j >= 0 && buf[j].z > t.z) { buf[j + 1] = buf[j]; j--; }
        buf[j + 1] = t;
    }
    if (n > 0 && copyout((void __user *)a1, buf, (size_t)n * sizeof buf[0]) < 0)
        return -EFAULT;
    return n;
}

/* Reposition a surface (DDR-710): owner or the compositor (CAP_SOVEREIGN). */
static long sys_surface_move(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    long r = 0;
    if (!s->used)                                                           r = -EINVAL;
    else if (s->owner_pid != current_thread->pid && !current_thread->is_sovereign) r = -EPERM;
    else { s->x = (int32_t)a2; s->y = (int32_t)a3; }
    spin_unlock_irqrestore(&g_surf_lock, fl);
    return r;
}

/* Raise a surface to the top of the stack and give it keyboard focus (DDR-708). */
static long sys_surface_raise(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    long r = 0;
    if (!s->used)                                                           r = -EINVAL;
    else if (s->owner_pid != current_thread->pid && !current_thread->is_sovereign) r = -EPERM;
    else {
        for (int i = 0; i < SURFACE_MAX; i++) g_surf[i].focused = 0;
        s->z = ++g_z_top;
        s->focused = 1;
    }
    spin_unlock_irqrestore(&g_surf_lock, fl);
    return r;
}

/* Compositor forwards a keystroke to a surface's private key ring (DDR-708). */
static long sys_surface_sendkey(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    long r = 0;
    if (!s->used)                                                           r = -EINVAL;
    else if (!current_thread->is_sovereign && s->owner_pid != current_thread->pid) r = -EPERM;
    else {
        uint8_t nh = (uint8_t)((s->kq_head + 1) % SURFACE_KQ);
        if (nh != s->kq_tail) {                  /* drop on full */
            s->kq[s->kq_head] = (uint8_t)a2;
            s->kq_head = nh;
        }
    }
    spin_unlock_irqrestore(&g_surf_lock, fl);
    return r;
}

/* The owning client drains a key from its surface's ring (DDR-708). */
static long sys_surface_getkey(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    long r;
    if (!s->used)                                r = -EINVAL;
    else if (s->owner_pid != current_thread->pid) r = -EPERM;
    else if (s->kq_head == s->kq_tail)           r = -1;    /* empty */
    else {
        r = (long)s->kq[s->kq_tail];
        s->kq_tail = (uint8_t)((s->kq_tail + 1) % SURFACE_KQ);
    }
    spin_unlock_irqrestore(&g_surf_lock, fl);
    return r;
}

/* Compositor-side: map a polled surface read-side into the caller; returns VA. */
static long sys_surface_cmap(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    return sys_surface_map_ro((int)a1);
}

/* Close a surface (DDR-711): free its PMM buffer and the slot. Owner or the
 * compositor (CAP_SOVEREIGN). When the OWNER closes (its AS is active), the VA
 * range is unmapped first so a stale access faults -> clean user-kill rather than
 * touching freed frames (ADR-021). A sovereign closing another process's surface
 * frees the frames without unmapping the owner's tables (not the active AS) — the
 * owner must not access a sovereign-closed surface. */
static long sys_surface_close(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    if (!s->used) { spin_unlock_irqrestore(&g_surf_lock, fl); return -EINVAL; }
    if (s->owner_pid != current_thread->pid && !current_thread->is_sovereign) {
        spin_unlock_irqrestore(&g_surf_lock, fl);
        return -EPERM;
    }
    int owner = (s->owner_pid == current_thread->pid);
    uint32_t npages = s->npages;
    uint64_t phys; unsigned order;
    surf_take_free(s, &phys, &order);              /* captures + clears the slot */
    spin_unlock_irqrestore(&g_surf_lock, fl);
    /* Unmap the owner's (active) view so a stale access faults -> clean user-kill
     * (ADR-021); the frames' PTE_SW_SHARED means AS teardown won't touch them, but
     * an explicit close should still drop the live mapping. A sovereign closing
     * another process's surface can't touch that (inactive) AS — the owner must
     * not access a sovereign-closed surface (its poll de-lists it). */
    if (owner) {
        uint64_t va = SURFACE_VA_BASE + (uint64_t)id * SURFACE_VA_SLOT;
        for (uint32_t i = 0; i < npages; i++)
            vmm_unmap(va + (uint64_t)i * PAGE_SIZE);
    }
    pmm_free_pages(phys, order);                   /* the ONE free, outside the lock */
    return 0;
}

/* DDR-729 — automatic reclamation: free every surface owned by an exiting pid.
 * Called from sched_exit. The owner's address space is being destroyed and no
 * thread will run in it again, so no unmap is needed; PTE_SW_SHARED guarantees
 * the subsequent vmm_destroy_address_space leaves these frames alone (no double
 * free). Frames go back to the buddy allocator OUTSIDE the lock. */
void surface_reap_pid(uint32_t pid) {
    for (int id = 0; id < SURFACE_MAX; id++) {
        uint64_t fl = spin_lock_irqsave(&g_surf_lock);
        struct surface *s = &g_surf[id];
        uint64_t phys = 0; unsigned order = 0;
        int live = (s->used && s->owner_pid == pid) ? (surf_take_free(s, &phys, &order), 1) : 0;
        spin_unlock_irqrestore(&g_surf_lock, fl);
        if (live)
            pmm_free_pages(phys, order);
    }
}

/* Resize a surface (DDR-711): swap in a fresh zeroed buffer of the new size,
 * keeping position/stack/focus/committed. Owner-only (it owns the draw buffer).
 * The owner re-maps (SYS_SURFACE_MAP) to draw into the new buffer. */
static long sys_surface_resize(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    int id = (int)a1;
    uint32_t w = (uint32_t)a2, h = (uint32_t)a3;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    if (w == 0 || h == 0 || w > SURFACE_DIM_MAX || h > SURFACE_DIM_MAX)
        return -EINVAL;
    /* Allocate + zero the new buffer BEFORE the lock. */
    uint64_t bytes = (uint64_t)w * h * 4;
    uint64_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t nphys = pmm_alloc_pages(order_for(npages));
    if (!nphys)
        return -ENOMEM;
    for (uint64_t i = 0; i < npages * PAGE_SIZE; i++)   /* zero (identity-mapped) */
        ((volatile uint8_t *)(uintptr_t)nphys)[i] = 0;

    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    if (!s->used || s->owner_pid != current_thread->pid) {
        long r = s->used ? -EPERM : -EINVAL;
        spin_unlock_irqrestore(&g_surf_lock, fl);
        pmm_free_pages(nphys, order_for(npages));      /* nothing to resize */
        return r;
    }
    uint64_t ophys = s->phys; uint32_t onpages = s->npages;
    s->phys = nphys; s->w = w; s->h = h; s->npages = (uint32_t)npages;
    spin_unlock_irqrestore(&g_surf_lock, fl);

    uint64_t va = SURFACE_VA_BASE + (uint64_t)id * SURFACE_VA_SLOT;
    for (uint32_t i = 0; i < onpages; i++)              /* drop the owner's old view */
        vmm_unmap(va + (uint64_t)i * PAGE_SIZE);
    pmm_free_pages(ophys, order_for(onpages));          /* free the old buffer */
    return 0;
}

/* Name a window (DDR-715). Owner-only; <=15 chars + NUL via copyinstr. */
static long sys_surface_set_title(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    /* Validate ownership under the lock, then copyinstr into a local (never under
     * the lock — a user-fault path must not run IRQ-off holding a spinlock); swap
     * the title in under the lock afterward. */
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    long r = 0;
    if (!s->used)                                r = -EINVAL;
    else if (s->owner_pid != current_thread->pid) r = -EPERM;
    spin_unlock_irqrestore(&g_surf_lock, fl);
    if (r)
        return r;
    char title[16];
    int faulted = (copyinstr(title, (const void __user *)a2, sizeof title, 0) < 0);
    if (faulted)
        title[0] = 0;                            /* leave the window untitled */
    fl = spin_lock_irqsave(&g_surf_lock);
    if (s->used && s->owner_pid == current_thread->pid)   /* re-check: still ours */
        for (unsigned i = 0; i < sizeof s->title; i++) s->title[i] = title[i];
    spin_unlock_irqrestore(&g_surf_lock, fl);
    return faulted ? -EFAULT : 0;
}

/* Push a typed event to a surface's ring (DDR-718): the compositor
 * (CAP_SOVEREIGN) or the owner. a3 packs (arg0<<16)|arg1. Drop-on-full. */
static long sys_surface_sendev(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    long r = 0;
    if (!s->used)                                                           r = -EINVAL;
    else if (!current_thread->is_sovereign && s->owner_pid != current_thread->pid) r = -EPERM;
    else {
        uint8_t nh = (uint8_t)((s->eq_head + 1) % SURFACE_EQ);
        if (nh != s->eq_tail) {
            s->eq[s->eq_head].type = (uint16_t)a2;
            s->eq[s->eq_head].arg0 = (uint16_t)((uint64_t)a3 >> 16);
            s->eq[s->eq_head].arg1 = (uint16_t)a3;
            s->eq_head = nh;
        }
    }
    spin_unlock_irqrestore(&g_surf_lock, fl);
    return r;
}

/* The owner drains one event (DDR-718); -EAGAIN when the ring is empty. */
static long sys_surface_getev(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX)
        return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&g_surf_lock);
    struct surface *s = &g_surf[id];
    struct surf_event ev;
    long r = 0;
    if (!s->used)                                r = -EINVAL;
    else if (s->owner_pid != current_thread->pid) r = -EPERM;
    else if (s->eq_head == s->eq_tail)           r = -EAGAIN;
    else ev = s->eq[s->eq_tail];                 /* snapshot; dequeue only if copyout ok */
    spin_unlock_irqrestore(&g_surf_lock, fl);
    if (r)
        return r;
    if (copyout((void __user *)a2, &ev, sizeof ev) < 0)
        return -EFAULT;                          /* leave the event queued */
    fl = spin_lock_irqsave(&g_surf_lock);
    if (s->used && s->owner_pid == current_thread->pid && s->eq_head != s->eq_tail)
        s->eq_tail = (uint8_t)((s->eq_tail + 1) % SURFACE_EQ);   /* advance past the delivered event */
    spin_unlock_irqrestore(&g_surf_lock, fl);
    return 0;
}

void sys_surface_register(void) {
    syscall_register(SYS_SURFACE_CREATE, sys_surface_create);
    syscall_register(SYS_SURFACE_MAP,    sys_surface_map);
    syscall_register(SYS_SURFACE_COMMIT, sys_surface_commit);
    syscall_register(SYS_SURFACE_POLL,   sys_surface_poll);
    syscall_register(SYS_SURFACE_CMAP,   sys_surface_cmap);
    syscall_register(SYS_SURFACE_RAISE,  sys_surface_raise);    /* DDR-708 */
    syscall_register(SYS_SURFACE_SENDKEY, sys_surface_sendkey);
    syscall_register(SYS_SURFACE_GETKEY, sys_surface_getkey);
    syscall_register(SYS_SURFACE_MOVE,   sys_surface_move);     /* DDR-710 */
    syscall_register(SYS_SURFACE_CLOSE,  sys_surface_close);    /* DDR-711 */
    syscall_register(SYS_SURFACE_RESIZE, sys_surface_resize);   /* DDR-711 */
    syscall_register(SYS_SURFACE_SET_TITLE, sys_surface_set_title); /* DDR-715 */
    syscall_register(SYS_SURFACE_SENDEV, sys_surface_sendev);   /* DDR-718 */
    syscall_register(SYS_SURFACE_GETEV,  sys_surface_getev);
}
