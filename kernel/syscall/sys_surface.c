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

#define SURFACE_MAX        16
#define SURFACE_DIM_MAX    512u                /* per-surface buffer <= 1 MiB (512*512*4) */
#define SURFACE_VA_BASE    0x8600000000ull     /* below the FB mapping (0x8700000000) */
#define SURFACE_VA_SLOT    0x100000ull         /* 1 MiB per surface id */

#define SURFACE_KQ 32                          /* per-surface key ring (DDR-708) */
#define SURFACE_EQ 8                           /* per-surface event ring (DDR-718) */
struct surf_event { uint16_t type, arg0, arg1; };   /* type 1 = RESIZE_REQ(w,h) */
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
};
static struct surface g_surf[SURFACE_MAX];
static int32_t g_z_top;                         /* monotonic stacking counter */

struct surface_info { uint32_t id, w, h; int32_t x, y, z; uint32_t focused; char title[16]; };

static unsigned order_for(uint64_t npages) {
    unsigned o = 0;
    while (((uint64_t)1 << o) < npages && o < 10) o++;
    return o;
}

static long sys_surface_create(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    uint32_t w = (uint32_t)a1, h = (uint32_t)a2;
    if (w == 0 || h == 0 || w > SURFACE_DIM_MAX || h > SURFACE_DIM_MAX)
        return -EINVAL;
    int id = -1;
    for (int i = 0; i < SURFACE_MAX; i++) if (!g_surf[i].used) { id = i; break; }
    if (id < 0)
        return -EMFILE;
    uint64_t bytes = (uint64_t)w * h * 4;
    uint64_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(order_for(npages));
    if (!phys)
        return -ENOMEM;
    for (uint64_t i = 0; i < npages * PAGE_SIZE; i++)   /* zero (identity-mapped) */
        ((volatile uint8_t *)(uintptr_t)phys)[i] = 0;
    struct surface *s = &g_surf[id];
    s->phys = phys; s->w = w; s->h = h; s->npages = (uint32_t)npages;
    s->owner_pid = current_thread->pid; s->x = s->y = 0;
    s->z = ++g_z_top; s->focused = 0;
    s->kq_head = s->kq_tail = 0;
    s->eq_head = s->eq_tail = 0;                /* empty event ring (DDR-718) */
    s->title[0] = 0;                            /* untitled until SET_TITLE (DDR-715) */
    s->used = 1; s->committed = 0;
    return id;
}

static long sys_surface_map(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    struct surface *s = &g_surf[id];
    if (s->owner_pid != current_thread->pid)
        return -EPERM;                          /* a client maps only its own surface */
    uint64_t va = SURFACE_VA_BASE + (uint64_t)id * SURFACE_VA_SLOT;
    uint64_t flags = VMM_USER | VMM_RW | VMM_NX;
    for (uint32_t i = 0; i < s->npages; i++)
        if (vmm_map_in(current_thread->cr3, va + (uint64_t)i * PAGE_SIZE,
                       s->phys + (uint64_t)i * PAGE_SIZE, flags) != 0)
            return -ENOMEM;
    return (long)va;
}

/* The compositor maps a client's surface to read it; owner check is skipped (the
 * compositor reads pixels it will composite, never gaining the owner's authority).
 * Distinguished from sys_surface_map by being the compositor path — same VA. */
static long sys_surface_map_ro(int id) {
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    struct surface *s = &g_surf[id];
    uint64_t va = SURFACE_VA_BASE + (uint64_t)id * SURFACE_VA_SLOT;
    uint64_t flags = VMM_USER | VMM_RW | VMM_NX;
    for (uint32_t i = 0; i < s->npages; i++)
        if (vmm_map_in(current_thread->cr3, va + (uint64_t)i * PAGE_SIZE,
                       s->phys + (uint64_t)i * PAGE_SIZE, flags) != 0)
            return -ENOMEM;
    return (long)va;
}

static long sys_surface_commit(long a1, long a2, long a3, long a4) {
    (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    struct surface *s = &g_surf[id];
    if (s->owner_pid != current_thread->pid)
        return -EPERM;
    s->x = (int32_t)a2; s->y = (int32_t)a3; s->committed = 1;
    return 0;
}

/* (id<0) -> POLL: list committed surfaces; (id>=0) -> compositor read-map.
 * One handler multiplexes so the compositor can map a surface it just polled.
 * a1 = buf/-(id+1)... kept as two distinct entry points below for clarity. */
static long sys_surface_poll(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    int max = (int)a2;
    if (max <= 0) return 0;
    if (max > SURFACE_MAX) max = SURFACE_MAX;
    struct surface_info buf[SURFACE_MAX];
    int n = 0;
    for (int i = 0; i < SURFACE_MAX && n < max; i++) {
        if (g_surf[i].used && g_surf[i].committed) {
            buf[n].id = (uint32_t)i; buf[n].w = g_surf[i].w; buf[n].h = g_surf[i].h;
            buf[n].x = g_surf[i].x;  buf[n].y = g_surf[i].y;
            buf[n].z = g_surf[i].z;  buf[n].focused = g_surf[i].focused;
            for (int c = 0; c < 16; c++) buf[n].title[c] = g_surf[i].title[c];
            n++;
        }
    }
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
static long sys_surface_move(long a1, long a2, long a3, long a4) {
    (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    if (g_surf[id].owner_pid != current_thread->pid && !current_thread->is_sovereign)
        return -EPERM;
    g_surf[id].x = (int32_t)a2;
    g_surf[id].y = (int32_t)a3;
    return 0;
}

/* Raise a surface to the top of the stack and give it keyboard focus (DDR-708). */
static long sys_surface_raise(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    if (g_surf[id].owner_pid != current_thread->pid && !current_thread->is_sovereign)
        return -EPERM;
    for (int i = 0; i < SURFACE_MAX; i++) g_surf[i].focused = 0;
    g_surf[id].z = ++g_z_top;
    g_surf[id].focused = 1;
    return 0;
}

/* Compositor forwards a keystroke to a surface's private key ring (DDR-708). */
static long sys_surface_sendkey(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    if (!current_thread->is_sovereign && g_surf[id].owner_pid != current_thread->pid)
        return -EPERM;
    struct surface *s = &g_surf[id];
    uint8_t nh = (uint8_t)((s->kq_head + 1) % SURFACE_KQ);
    if (nh != s->kq_tail) {                      /* drop on full */
        s->kq[s->kq_head] = (uint8_t)a2;
        s->kq_head = nh;
    }
    return 0;
}

/* The owning client drains a key from its surface's ring (DDR-708). */
static long sys_surface_getkey(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    struct surface *s = &g_surf[id];
    if (s->owner_pid != current_thread->pid)
        return -EPERM;
    if (s->kq_head == s->kq_tail)
        return -1;                               /* empty */
    uint8_t c = s->kq[s->kq_tail];
    s->kq_tail = (uint8_t)((s->kq_tail + 1) % SURFACE_KQ);
    return (long)c;
}

/* Compositor-side: map a polled surface read-side into the caller; returns VA. */
static long sys_surface_cmap(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    return sys_surface_map_ro((int)a1);
}

/* Close a surface (DDR-711): free its PMM buffer and the slot. Owner or the
 * compositor (CAP_SOVEREIGN). When the OWNER closes (its AS is active), the VA
 * range is unmapped first so a stale access faults -> clean user-kill rather than
 * touching freed frames (ADR-021). A sovereign closing another process's surface
 * frees the frames without unmapping the owner's tables (not the active AS) — the
 * owner must not access a sovereign-closed surface. */
static long sys_surface_close(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    struct surface *s = &g_surf[id];
    if (s->owner_pid != current_thread->pid && !current_thread->is_sovereign)
        return -EPERM;
    if (s->owner_pid == current_thread->pid) {     /* unmap the owner's (active) view */
        uint64_t va = SURFACE_VA_BASE + (uint64_t)id * SURFACE_VA_SLOT;
        for (uint32_t i = 0; i < s->npages; i++)
            vmm_unmap(va + (uint64_t)i * PAGE_SIZE);
    }
    pmm_free_pages(s->phys, order_for(s->npages));
    for (unsigned i = 0; i < sizeof *s; i++) ((uint8_t *)s)[i] = 0;   /* clears used */
    return 0;
}

/* Resize a surface (DDR-711): swap in a fresh zeroed buffer of the new size,
 * keeping position/stack/focus/committed. Owner-only (it owns the draw buffer).
 * The owner re-maps (SYS_SURFACE_MAP) to draw into the new buffer. */
static long sys_surface_resize(long a1, long a2, long a3, long a4) {
    (void)a4;
    int id = (int)a1;
    uint32_t w = (uint32_t)a2, h = (uint32_t)a3;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    struct surface *s = &g_surf[id];
    if (s->owner_pid != current_thread->pid)
        return -EPERM;
    if (w == 0 || h == 0 || w > SURFACE_DIM_MAX || h > SURFACE_DIM_MAX)
        return -EINVAL;
    uint64_t bytes = (uint64_t)w * h * 4;
    uint64_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t nphys = pmm_alloc_pages(order_for(npages));
    if (!nphys)
        return -ENOMEM;
    for (uint64_t i = 0; i < npages * PAGE_SIZE; i++)   /* zero (identity-mapped) */
        ((volatile uint8_t *)(uintptr_t)nphys)[i] = 0;
    uint64_t va = SURFACE_VA_BASE + (uint64_t)id * SURFACE_VA_SLOT;
    for (uint32_t i = 0; i < s->npages; i++)            /* drop the owner's old view */
        vmm_unmap(va + (uint64_t)i * PAGE_SIZE);
    pmm_free_pages(s->phys, order_for(s->npages));
    s->phys = nphys; s->w = w; s->h = h; s->npages = (uint32_t)npages;
    return 0;
}

/* Name a window (DDR-715). Owner-only; <=15 chars + NUL via copyinstr. */
static long sys_surface_set_title(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    struct surface *s = &g_surf[id];
    if (s->owner_pid != current_thread->pid)
        return -EPERM;
    if (copyinstr(s->title, (const void __user *)a2, sizeof s->title, 0) < 0) {
        s->title[0] = 0;
        return -EFAULT;
    }
    return 0;
}

/* Push a typed event to a surface's ring (DDR-718): the compositor
 * (CAP_SOVEREIGN) or the owner. a3 packs (arg0<<16)|arg1. Drop-on-full. */
static long sys_surface_sendev(long a1, long a2, long a3, long a4) {
    (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    if (!current_thread->is_sovereign && g_surf[id].owner_pid != current_thread->pid)
        return -EPERM;
    struct surface *s = &g_surf[id];
    uint8_t nh = (uint8_t)((s->eq_head + 1) % SURFACE_EQ);
    if (nh != s->eq_tail) {
        s->eq[s->eq_head].type = (uint16_t)a2;
        s->eq[s->eq_head].arg0 = (uint16_t)((uint64_t)a3 >> 16);
        s->eq[s->eq_head].arg1 = (uint16_t)a3;
        s->eq_head = nh;
    }
    return 0;
}

/* The owner drains one event (DDR-718); -EAGAIN when the ring is empty. */
static long sys_surface_getev(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    int id = (int)a1;
    if (id < 0 || id >= SURFACE_MAX || !g_surf[id].used)
        return -EINVAL;
    struct surface *s = &g_surf[id];
    if (s->owner_pid != current_thread->pid)
        return -EPERM;
    if (s->eq_head == s->eq_tail)
        return -EAGAIN;
    struct surf_event ev = s->eq[s->eq_tail];
    if (copyout((void __user *)a2, &ev, sizeof ev) < 0)
        return -EFAULT;                          /* leave the event queued */
    s->eq_tail = (uint8_t)((s->eq_tail + 1) % SURFACE_EQ);
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
