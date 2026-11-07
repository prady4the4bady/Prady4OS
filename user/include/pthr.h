/* user/include/pthr.h — a native pthreads subset on psync.h (DDR-1197; C1.4 P4).
 *
 * Header-only and freestanding, like psync.h: no libc, no writable globals
 * (user.ld). Threads are SYS_THREAD_CREATE threads (DDR-1181); join sleeps on
 * the CLEARTID word; the primitives are psync.h's (gated by smoke-thread
 * SA-SD). Raw kernel return values, no errno: where POSIX returns a positive
 * error number, pthr_* returns its negative.
 *
 * Gated by smoke-thread arms PA, PJ, PR, PD, PG, PK and PO (user/pthrtest.c,
 * DDR-1197 §4 and Amendment 1). Emulated (QEMU TCG) only; not verified on real
 * hardware. Memory-order reasoning rests on x86-64 TSO, as psync.h's does.
 *
 * Contract (DDR-1197 §3, §6):
 *  - Freestanding programs only (D3): build with -DPTHR_FREESTANDING, link with
 *    user/user.ld, never with the musl runtime.
 *  - pthr_init() exactly once, from the main thread, before any other call.
 *  - After fork() in a process that called pthr_init(), the child may call no
 *    pthr_* function and no primitive another thread could have held; it may
 *    execve or exit (rt_lock may be held by a thread that does not exist there).
 *  - Joining a detached thread is undefined (as in POSIX): any later reap may
 *    unmap it. pthr_join never reaps.
 *  - Inherited kernel divergences (DDR-1181 §3.6): fd tables and signal
 *    dispositions are per thread; execve with more than one thread is -EBUSY.
 *
 * The guard page under each thread stack is read-only: the kernel refuses
 * PROT_NONE (DDR-1031 §3). Writes into it fault; reads do not, and a frame
 * larger than one page can step over it (DDR-1197 Amendment 1 items 9-11).
 * Stack size is PTHR_STACK_MIN (16 KiB) to PTHR_STACK_MAX (64 MiB).
 *
 * Not provided: cancellation, pthread_sigmask, rwlocks, spinlocks, robust or PI
 * mutexes, timed waits, ELF __thread (C1.6), attributes other than stack size
 * and detach state, recursive and error-checking mutexes.
 */
#ifndef PTHR_H
#define PTHR_H

#ifndef PTHR_FREESTANDING
#error "pthr.h is for freestanding programs linked with user/user.ld (DDR-1197 D3): build with -DPTHR_FREESTANDING"
#endif
#ifdef PTHREAD_CREATE_JOINABLE
#error "pthr.h cannot be used with musl's pthread.h (DDR-1197 D3)"
#endif

#include <stdint.h>
#include "pradyos.h"
#include "psync.h"

#define PTHR_KEYS_MAX              32
#define PTHR_DESTRUCTOR_ITERATIONS 4
#define PTHR_PAGE                  4096UL
#define PTHR_STACK_DEFAULT         (64UL * 1024)
#define PTHR_STACK_MIN             (16UL * 1024)
#define PTHR_STACK_MAX             (64UL * 1024 * 1024)   /* bounds the length arithmetic */
#define PTHR_GEN_MASK              0xFFFFFFu
#define PTHR_CREATE_TRIES          4      /* maps retried when a fork lands mid-create */

#define PTHR_EAGAIN   11
#define PTHR_EACCES   13
#define PTHR_EINVAL   22
#define PTHR_EDEADLK  35

#define PTHR__PROT_READ  0x1
#define PTHR__PROT_RW    0x3
#define PTHR__MAP_PRIV   0x02
#define PTHR__MAP_ANON   0x20

typedef void *(*pthr_start_fn)(void *);
typedef void (*pthr_dtor_fn)(void *);
typedef uint32_t pthr_key_t;               /* (generation << 8) | slot */

struct pthr_rt;

struct pthr_spec {
    uint32_t gen;                          /* the key generation it was set under */
    uint32_t pad;
    void    *val;
};

/* The thread block: one page at the top of the thread's mapping. FS points here. */
struct pthr {
    struct pthr      *self;                /* offset 0: %fs:0 (x86-64 TLS ABI) */
    volatile uint32_t ctid;                /* the kernel writes 0 here at exit */
    volatile uint32_t detached;
    long              tid;                 /* the kernel's thread id */
    pthr_start_fn     start;
    void             *arg;
    void *volatile    ret;
    uintptr_t         map_base, map_len;   /* for munmap: guard + stack + block */
    struct pthr_rt   *rt;
    struct pthr      *next;                /* detached list link, under rt_lock */
    struct pthr_spec  spec[PTHR_KEYS_MAX];
};
typedef struct pthr *pthr_t;

/* The process runtime: one mapped page that every thread block points to. */
struct pthr_rt {
    pmutex            lock;                /* rt_lock: keys and the detached list */
    volatile uint32_t key_used;            /* bitmap of slots in use */
    volatile uint32_t gen[PTHR_KEYS_MAX];
    pthr_dtor_fn      dtor[PTHR_KEYS_MAX];
    struct pthr      *detached;            /* the only owner of a detached block */
    struct pthr      *main;                /* the main thread's block */
};

typedef struct { unsigned long stacksize; int detached; } pthr_attr_t;
typedef struct { volatile uint32_t st; } pthr_once_t;   /* 0 not run, 1 running, 2 done */
#define PTHR_ONCE_INIT { 0 }

/* ---- syscalls ------------------------------------------------------------ */

static inline long pthr__sys6(long n, long a1, long a2, long a3,
                              long a4, long a5, long a6) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long pthr__mmap(unsigned long len) {
    return pthr__sys6(SYS_MMAP, 0, (long)len, PTHR__PROT_RW,
                      PTHR__MAP_PRIV | PTHR__MAP_ANON, -1, 0);
}
static inline long pthr__munmap(uintptr_t base, unsigned long len) {
    return pradyos_syscall(SYS_MUNMAP, (long)base, (long)len, 0);
}

static inline pthr_t pthr_self(void) {
    struct pthr *p;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(p));
    return p;
}

/* ---- init ---------------------------------------------------------------- */

/* Call exactly once, from the main thread, before any other pthr_* call.
 * It does not read %fs first: FS base is not known to be 0 at program start
 * (DDR-1197 §3.2, #110). */
static inline long pthr_init(void) {
    long r = pthr__mmap(PTHR_PAGE);
    if (r < 0) return r;
    long m = pthr__mmap(PTHR_PAGE);
    if (m < 0) { pthr__munmap((uintptr_t)r, PTHR_PAGE); return m; }
    struct pthr_rt *rt = (struct pthr_rt *)r;
    struct pthr *mb = (struct pthr *)m;
    pmutex_init(&rt->lock);
    rt->main = mb;
    mb->self = mb;
    mb->ctid = 1;
    mb->tid = pradyos_syscall(SYS_GETPID, 0, 0, 0);
    mb->map_base = (uintptr_t)m;
    mb->map_len = PTHR_PAGE;
    mb->rt = rt;
    return pradyos_syscall(SYS_SET_TLS, m, 0, 0);
}

/* ---- keys (DDR-1197 §3.5): generation-tagged ------------------------------ */

static inline long pthr_key_create(pthr_key_t *k, pthr_dtor_fn d) {
    struct pthr_rt *rt = pthr_self()->rt;
    pmutex_lock(&rt->lock);
    for (uint32_t s = 0; s < PTHR_KEYS_MAX; s++) {
        if (rt->key_used & (1u << s)) continue;
        rt->dtor[s] = d;
        __atomic_or_fetch(&rt->key_used, 1u << s, __ATOMIC_RELEASE);
        uint32_t g = __atomic_load_n(&rt->gen[s], __ATOMIC_ACQUIRE) & PTHR_GEN_MASK;
        pmutex_unlock(&rt->lock);
        *k = (g << 8) | s;
        return 0;
    }
    pmutex_unlock(&rt->lock);
    return -PTHR_EAGAIN;
}

/* Frees the slot and bumps its generation; runs no destructors (POSIX). */
static inline long pthr_key_delete(pthr_key_t k) {
    struct pthr_rt *rt = pthr_self()->rt;
    uint32_t s = k & 0xFF;
    if (s >= PTHR_KEYS_MAX) return -PTHR_EINVAL;
    pmutex_lock(&rt->lock);
    if (!(rt->key_used & (1u << s)) ||
        (__atomic_load_n(&rt->gen[s], __ATOMIC_ACQUIRE) & PTHR_GEN_MASK) != (k >> 8)) {
        pmutex_unlock(&rt->lock);
        return -PTHR_EINVAL;
    }
    __atomic_and_fetch(&rt->key_used, ~(1u << s), __ATOMIC_RELEASE);
    __atomic_add_fetch(&rt->gen[s], 1, __ATOMIC_RELEASE);
    rt->dtor[s] = 0;
    pmutex_unlock(&rt->lock);
    return 0;
}

static inline long pthr_setspecific(pthr_key_t k, const void *v) {
    uint32_t s = k & 0xFF;
    if (s >= PTHR_KEYS_MAX) return -PTHR_EINVAL;
    struct pthr *t = pthr_self();
    t->spec[s].gen = k >> 8;
    t->spec[s].val = (void *)v;
    return 0;
}

static inline void *pthr_getspecific(pthr_key_t k) {
    uint32_t s = k & 0xFF;
    if (s >= PTHR_KEYS_MAX) return 0;
    struct pthr *t = pthr_self();
    if (t->spec[s].gen != (k >> 8)) return 0;      /* set under a deleted key */
    return t->spec[s].val;
}

/* At exit: up to PTHR_DESTRUCTOR_ITERATIONS rounds. A value runs its key's
 * destructor only if it was set under the slot's current generation in an
 * in-use slot; a stale value is dropped. */
static inline void pthr__run_dtors(struct pthr *t) {
    struct pthr_rt *rt = t->rt;
    for (int it = 0; it < PTHR_DESTRUCTOR_ITERATIONS; it++) {
        int ran = 0;
        for (uint32_t s = 0; s < PTHR_KEYS_MAX; s++) {
            void *v = t->spec[s].val;
            if (!v) continue;
            t->spec[s].val = 0;
            uint32_t used = __atomic_load_n(&rt->key_used, __ATOMIC_ACQUIRE);
            uint32_t g = __atomic_load_n(&rt->gen[s], __ATOMIC_ACQUIRE) & PTHR_GEN_MASK;
            if (!(used & (1u << s)) || t->spec[s].gen != g) continue;
            pthr_dtor_fn d = rt->dtor[s];
            if (d) { d(v); ran = 1; }
        }
        if (!ran) break;
    }
}

/* ---- exit, start ---------------------------------------------------------- */

__attribute__((noreturn)) static inline void pthr_exit(void *ret) {
    struct pthr *t = pthr_self();
    pthr__run_dtors(t);
    __atomic_store_n(&t->ret, ret, __ATOMIC_RELEASE);
    /* The thread never touches its stack after this: the kernel clears ctid
     * after the last user instruction, so a joiner may then unmap it. */
    pradyos_syscall(SYS_THREAD_EXIT, 0, 0, 0);
    for (;;) { }
}

__attribute__((used, noinline, noreturn)) static void pthr__start_c(struct pthr *t) {
    pthr_exit(t->start(t->arg));
}

/* The kernel enters here with RSP = stack_top (16-byte aligned, nothing
 * pushed) and RDI = the thread block. SysV wants RSP = 8 (mod 16) at a
 * function's first instruction, so a call from an aligned RSP gives exactly
 * that (DDR-1197 §3.3). A naked function may hold only basic asm. */
__attribute__((naked, used)) static void pthr__entry(void) {
    __asm__("xorl %ebp, %ebp\n\t"
            "andq $-16, %rsp\n\t"
            "call pthr__start_c\n\t"
            "ud2");
}

/* ---- detach, reap ---------------------------------------------------------- */

static inline void pthr__list_add(struct pthr_rt *rt, struct pthr *t) {
    pmutex_lock(&rt->lock);
    t->next = rt->detached;
    rt->detached = t;
    pmutex_unlock(&rt->lock);
}

/* Unmaps every detached block whose ctid reads 0; returns how many. */
static inline long pthr_reap(void) {
    struct pthr_rt *rt = pthr_self()->rt;
    long n = 0;
    pmutex_lock(&rt->lock);
    struct pthr **pp = &rt->detached;
    while (*pp) {
        struct pthr *t = *pp;
        if (__atomic_load_n(&t->ctid, __ATOMIC_ACQUIRE) == 0) {
            *pp = t->next;                 /* read before the unmap */
            if (pthr__munmap(t->map_base, t->map_len) == 0) n++;
        } else {
            pp = &t->next;
        }
    }
    pmutex_unlock(&rt->lock);
    return n;
}

static inline long pthr_detach(pthr_t t) {
    if (__atomic_exchange_n(&t->detached, 1u, __ATOMIC_ACQ_REL)) return -PTHR_EINVAL;
    pthr__list_add(pthr_self()->rt, t);
    return 0;
}

/* ---- create, join ---------------------------------------------------------- */

static inline long pthr_create(pthr_t *out, const pthr_attr_t *a,
                               pthr_start_fn fn, void *arg) {
    struct pthr *self = pthr_self();
    pthr_reap();
    unsigned long ss = (a && a->stacksize) ? a->stacksize : PTHR_STACK_DEFAULT;
    int det = (a && a->detached) ? 1 : 0;   /* t->detached may change once t runs */
    /* Bounded first, so the rounding and the length below cannot wrap. */
    if (ss < PTHR_STACK_MIN || ss > PTHR_STACK_MAX) return -PTHR_EINVAL;
    ss = (ss + PTHR_PAGE - 1) & ~(PTHR_PAGE - 1);
    unsigned long len = PTHR_PAGE + ss + PTHR_PAGE;   /* guard, stack, block */
    /* The guard is READ-ONLY, not PROT_NONE: the kernel refuses PROT_NONE
     * (DDR-1031 §3, it would collide with stack demand paging). What it
     * catches, and what not (DDR-1197 Amendment 1 items 9-11):
     *  - a stack overflow whose first write lands in this page (each call
     *    and push writes) faults and ends the process;
     *  - a frame larger than one page can step over the guard, and its
     *    first write then lands below it, in whatever is mapped there (often
     *    another thread's block): not caught. The build does not use
     *    -fstack-clash-protection;
     *  - a stray read below the stack is not caught.
     * A fork in another thread during these steps (Amendment 1 item 11):
     *  - the region is mapped writable, so a fork marks all of it COW and the
     *    child never shares a frame the parent can still write;
     *  - a fork between the mmap and the guard's mprotect would leave the guard
     *    COW, so a write would copy it instead of faulting. The second mprotect
     *    detects that: it asks for RW on the stack and block, which changes no
     *    permission, and the kernel answers -EACCES exactly when a page there
     *    is COW, i.e. when a fork ran since the mmap. Then the region is
     *    unmapped and mapped again, at most PTHR_CREATE_TRIES times;
     *  - a fork after that check shares the read-only guard as it is (never
     *    writable) and COWs the stack and block. */
    long base = -PTHR_EAGAIN;
    for (int tries = 0; tries < PTHR_CREATE_TRIES; tries++) {
        base = pthr__mmap(len);
        if (base < 0) return base;
        long pr = pradyos_syscall(SYS_MPROTECT, base, (long)PTHR_PAGE, PTHR__PROT_READ);
        if (pr != 0) { pthr__munmap((uintptr_t)base, len); return pr; }
        long ck = pradyos_syscall(SYS_MPROTECT, base + (long)PTHR_PAGE,
                                  (long)(len - PTHR_PAGE), PTHR__PROT_RW);
        if (ck == 0) break;
        pthr__munmap((uintptr_t)base, len);
        if (ck != -PTHR_EACCES) return ck;
        base = -PTHR_EAGAIN;               /* a fork ran mid-create: map again */
    }
    if (base < 0) return base;
    uintptr_t stack_top = (uintptr_t)base + PTHR_PAGE + ss;
    struct pthr *t = (struct pthr *)stack_top;        /* the block sits above the stack */
    t->self = t;
    t->ctid = 1;                         /* before the syscall: the thread may exit first */
    t->detached = (uint32_t)det;
    t->start = fn;
    t->arg = arg;
    t->ret = 0;
    t->map_base = (uintptr_t)base;
    t->map_len = len;
    t->rt = self->rt;
    /* rt_lock is held from before the thread exists until t is complete and,
     * if detached, listed (DDR-1197 Amendment 1 item 11). Meanwhile a
     * pthr_detach(pthr_self()) by the new thread waits in pthr__list_add, and
     * no reap can see or unmap t. A thread created joinable that detaches
     * itself lists itself; the creator lists only when `det` was set at
     * create, so t is listed exactly once. */
    struct pthr_rt *rt = self->rt;
    pmutex_lock(&rt->lock);
    long tid = pthr__sys6(SYS_THREAD_CREATE, (long)(uintptr_t)pthr__entry, (long)stack_top,
                          (long)(uintptr_t)t, (long)(uintptr_t)t, (long)(uintptr_t)&t->ctid, 0);
    if (tid < 0) {
        pmutex_unlock(&rt->lock);
        pthr__munmap((uintptr_t)base, len);
        return tid;
    }
    t->tid = tid;
    if (det) { t->next = rt->detached; rt->detached = t; }
    pmutex_unlock(&rt->lock);
    *out = t;                            /* t may be gone already if detached */
    return 0;
}

/* Waits on the CLEARTID word WITHOUT FUTEX_PRIVATE_FLAG: the kernel's CLEARTID
 * wake uses the shared key, which equals the private key only outside
 * MAP_SHARED memory (DDR-1197 §1, §3.3). Never reaps. */
static inline long pthr_join(pthr_t t, void **ret) {
    if (t == pthr_self()) return -PTHR_EDEADLK;
    uint32_t c;
    if ((c = __atomic_load_n(&t->ctid, __ATOMIC_ACQUIRE)) != 0) { }   /* MUTANT R1: join does not wait */
    void *r = __atomic_load_n(&t->ret, __ATOMIC_ACQUIRE);
    if (ret) *ret = r;
    pthr__munmap(t->map_base, t->map_len);
    return 0;
}

/* ---- once (DDR-1197 §3.6) ---------------------------------------------------- */

static inline void pthr_once(pthr_once_t *o, void (*fn)(void)) {
    if (__atomic_load_n(&o->st, __ATOMIC_ACQUIRE) == 2) return;
    uint32_t z = 0;
    if (__atomic_compare_exchange_n(&o->st, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(&o->st, 2, __ATOMIC_RELEASE);
        ps_futex(&o->st, PS_FUTEX_WAKE | PS_FUTEX_PRIVATE, PS_WAKE_ALL);
        return;
    }
    while (__atomic_load_n(&o->st, __ATOMIC_ACQUIRE) != 2)
        ps_futex(&o->st, PS_FUTEX_WAIT | PS_FUTEX_PRIVATE, 1);
}

/* ---- the primitives: psync.h, already gated (SA-SD, Y1-Y6) ------------------- */

typedef pmutex   pthr_mutex;
typedef pcond    pthr_cond;
typedef pbarrier pthr_barrier;
typedef psem     pthr_sem;
#define PTHR_MUTEX_INITIALIZER { 0, PS_FUTEX_PRIVATE }

static inline void pthr_mutex_init(pthr_mutex *m)   { pmutex_init(m); }
static inline void pthr_mutex_lock(pthr_mutex *m)   { pmutex_lock(m); }
static inline void pthr_mutex_unlock(pthr_mutex *m) { pmutex_unlock(m); }
static inline void pthr_cond_init(pthr_cond *c)     { pcond_init(c); }
static inline long pthr_cond_wait(pthr_cond *c, pthr_mutex *m) { return pcond_wait(c, m); }
static inline void pthr_cond_signal(pthr_cond *c)    { pcond_signal(c); }
static inline void pthr_cond_broadcast(pthr_cond *c) { pcond_broadcast(c); }
static inline void pthr_barrier_init(pthr_barrier *b, uint32_t n) { pbarrier_init(b, n); }
static inline int  pthr_barrier_wait(pthr_barrier *b) { return pbarrier_wait(b); }
static inline void pthr_sem_init(pthr_sem *s, uint32_t v) { psem_init(s, v); }
static inline void pthr_sem_wait(pthr_sem *s) { psem_wait(s); }
static inline void pthr_sem_post(pthr_sem *s) { psem_post(s); }

#endif /* PTHR_H */
