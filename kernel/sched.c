/* kernel/sched.c — preemptive round-robin kernel scheduler (Phase 2c). */
#include "sched.h"
#include "kheap.h"
#include "console.h"

#define STACK_SIZE   16384u
#define QUANTUM      2u           /* ticks per slice (PIT @100Hz -> 20 ms) */

extern void context_switch(uint64_t *save_rsp, uint64_t load_rsp);  /* context.asm */

struct tcb *current_thread;       /* NULL until sched_init (safe: sched_tick checks) */
static struct tcb idle_tcb;
static uint32_t next_tid = 1;

/* First code a freshly-created thread runs (entered via context_switch's RET).
 * current_thread is already the new thread (set by schedule before switching). */
static void thread_trampoline(void) {
    current_thread->entry(current_thread->arg);
    current_thread->state = THREAD_DONE;
    for (;;)
        __asm__ volatile("hlt");
}

void sched_init(void) {
    idle_tcb.tid = 0;
    idle_tcb.name = "idle";
    idle_tcb.state = THREAD_RUNNING;
    idle_tcb.quantum = idle_tcb.quantum_reset = QUANTUM;
    idle_tcb.next = &idle_tcb;     /* ring of one */
    current_thread = &idle_tcb;
}

struct tcb *sched_create(thread_fn entry, void *arg, const char *name) {
    struct tcb *t = (struct tcb *)kmalloc(sizeof(struct tcb));
    if (!t)
        return 0;
    uint64_t base = (uint64_t)(uintptr_t)kmalloc(STACK_SIZE);
    if (!base) {
        kfree(t);
        return 0;
    }

    t->kstack_base = base;
    t->tid = next_tid++;
    t->state = THREAD_READY;
    t->quantum = t->quantum_reset = QUANTUM;
    t->entry = entry;
    t->arg = arg;
    t->name = name;

    /* Seed the stack with a context_switch frame whose RET enters the
     * trampoline, with RFLAGS = IF set so the thread runs interruptible. */
    uint64_t *sp = (uint64_t *)(uintptr_t)(base + STACK_SIZE);
    *--sp = (uint64_t)(uintptr_t)thread_trampoline;  /* return address */
    *--sp = 0;            /* rbx */
    *--sp = 0;            /* rbp */
    *--sp = 0;            /* r12 */
    *--sp = 0;            /* r13 */
    *--sp = 0;            /* r14 */
    *--sp = 0;            /* r15 */
    *--sp = 0x202;        /* rflags: IF | reserved bit 1 */
    t->rsp = (uint64_t)(uintptr_t)sp;

    /* Insert into the ring after the current thread. */
    t->next = current_thread->next;
    current_thread->next = t;
    return t;
}

/* Switch to the next thread in the ring (round-robin). */
static void schedule(void) {
    struct tcb *prev = current_thread;
    struct tcb *next = prev->next;
    if (next == prev)
        return;                    /* nothing else to run */
    if (prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;
    next->state = THREAD_RUNNING;
    current_thread = next;
    context_switch(&prev->rsp, next->rsp);
    /* resumed here later, as `prev`, when scheduled again */
}

void sched_tick(void) {
    if (!current_thread)
        return;                    /* scheduler not up yet */
    if (current_thread->quantum > 0)
        current_thread->quantum--;
    if (current_thread->quantum == 0) {
        current_thread->quantum = current_thread->quantum_reset;
        schedule();
    }
}

void yield(void) {
    if (!current_thread)
        return;
    current_thread->quantum = current_thread->quantum_reset;
    schedule();
}
