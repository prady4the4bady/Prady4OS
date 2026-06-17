/* kernel/sched.h — NEXUS round-robin scheduler + thread control block (2c).
 *
 * Minimal preemptive multitasking: a circular ready ring of kernel threads,
 * each with its own stack, switched by the PIT tick when its quantum expires.
 * The 3-lane adaptive scheduler (NAS) from the Layer-2 board layers on later.
 */
#pragma once
#include <stdint.h>

typedef void (*thread_fn)(void *);

enum thread_state { THREAD_READY = 0, THREAD_RUNNING = 1, THREAD_DONE = 2 };

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
};

void        sched_init(void);                                   /* boot ctx -> idle thread */
struct tcb *sched_create(thread_fn entry, void *arg, const char *name);
void        sched_tick(void);                                   /* from the timer IRQ      */
void        yield(void);                                        /* cooperative switch      */

extern struct tcb *current_thread;
