/* kernel/sched.h — NEXUS round-robin scheduler + thread control block (2c).
 *
 * Minimal preemptive multitasking: a circular ready ring of kernel threads,
 * each with its own stack, switched by the PIT tick when its quantum expires.
 * The 3-lane adaptive scheduler (NAS) from the Layer-2 board layers on later.
 */
#pragma once
#include <stdint.h>
#include "cap.h"

typedef void (*thread_fn)(void *);

/* Default per-thread filesystem write budget (bytes). Caps how much a single
 * thread can write through the VFS so a buggy/hostile consumer cannot exhaust
 * the block device. Generous for tools, far below any real disk. */
#define FS_WRITE_BUDGET_DEFAULT (1u << 20)   /* 1 MiB */

enum thread_state {
    THREAD_READY = 0,
    THREAD_RUNNING = 1,
    THREAD_DONE = 2,
    THREAD_BLOCKED = 3
};

struct tcb {
    uint64_t   rsp;            /* saved stack pointer (offset 0; asm relies on it) */
    uint64_t   kstack_base;    /* base of the thread's kernel stack               */
    uint32_t   tid;
    uint32_t   state;
    uint32_t   quantum;        /* ticks left in the current slice                 */
    uint32_t   quantum_reset;  /* slice length in ticks                           */
    struct tcb *next;          /* circular ready ring                             */
    thread_fn  entry;
    void      *arg;
    const char *name;
    struct cap_table *caps;    /* per-thread (process) capability table */
    uint64_t   fs_write_budget; /* remaining VFS write allowance (bytes)  */

    /* Ring-3 (user) thread metadata — zero/unused for pure kernel threads. */
    uint32_t   is_user;
    uint32_t   pid;
    uint64_t   user_rip;       /* user-mode entry virtual address */
    uint64_t   user_stack;     /* user-mode stack top (virtual)   */
    uint64_t   user_arg;       /* value delivered to the user in RDI */
    uint64_t   cr3;            /* process page-table root; 0 == kernel master AS */
};

void        sched_init(void);                                   /* boot ctx -> idle thread */
struct tcb *sched_create(thread_fn entry, void *arg, const char *name);
struct tcb *sched_create_user(const char *name, uint64_t user_rip, uint64_t user_stack);
void        sched_tick(void);                                   /* from the timer IRQ      */
void        yield(void);                                        /* cooperative switch      */
void        sched_block(void);                                  /* block current; switch away */
void        sched_unblock(struct tcb *t);                       /* mark a blocked thread ready */
void        sched_exit(void);                                   /* terminate current thread   */

extern struct tcb *current_thread;
