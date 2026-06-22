/* kernel/sched.h — NEXUS round-robin scheduler + thread control block (2c).
 *
 * Minimal preemptive multitasking: a circular ready ring of kernel threads,
 * each with its own stack, switched by the PIT tick when its quantum expires.
 * The 3-lane adaptive scheduler (NAS) from the Layer-2 board layers on later.
 */
#pragma once
#include <stdint.h>
#include "cap.h"
#include "fd.h"
#include "regs.h"

typedef void (*thread_fn)(void *);

/* Default per-thread filesystem write budget (bytes). Caps how much a single
 * thread can write through the VFS so a buggy/hostile consumer cannot exhaust
 * the block device. Generous for tools, far below any real disk. */
#define FS_WRITE_BUDGET_DEFAULT (1u << 20)   /* 1 MiB */

enum thread_state {
    THREAD_READY = 0,
    THREAD_RUNNING = 1,
    THREAD_DONE = 2,           /* finished kernel thread (no AS to reclaim)     */
    THREAD_BLOCKED = 3,
    THREAD_ZOMBIE = 4          /* exited user proc awaiting wait4/reaper (5b-9) */
};

/* Anonymous mmap region tracking (5b, ADR-022). A fixed table per process;
 * npages == 0 marks a free slot. Lets sys_munmap find + free a region and
 * (later) a reaper reclaim everything an exited process mapped. */
#define VM_AREA_MAX 64
struct vm_area {
    uint64_t base;     /* page-aligned start in the user mmap arena */
    uint64_t npages;   /* length in pages; 0 = free slot            */
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

    struct fd_table fdt;       /* per-process file descriptors (5b, ADR-022) */
    int        root_mnt;       /* mount paths resolve against (-1 = none)    */
    cap_t      fs_cap;         /* FS capability granted at load (5b)         */

    struct vm_area vma[VM_AREA_MAX];  /* anonymous mmap regions (5b)         */
    uint64_t   mmap_next;      /* bump pointer for addr==NULL mmaps           */

    /* Process model (5b slice 8+). Appended at struct end so the ~30 includers
     * of sched.h keep their field offsets. */
    uint32_t   parent_pid;     /* creating process pid; 0 for kernel threads  */
    int64_t    fork_retval;    /* child's fork() return (0); -1 when unset     */
    int        exit_status;    /* set by sched_exit; collected by wait4 (5b-9) */
    struct tcb *waiter;        /* parent blocked in wait4 on this thread, or 0 */

    /* POSIX signals (PROC-C). */
    uint64_t   sig_pending;      /* bitmap of pending signal numbers          */
    uint64_t   sig_handlers[32]; /* ring-3 handler VA per signal (NSIG=32)    */
    struct regs sig_saved;       /* register snapshot restored by sigreturn   */
    int        sig_active;       /* 1 while a signal handler is running        */
};

void        sched_init(void);                                   /* boot ctx -> idle thread */
struct tcb *sched_create(thread_fn entry, void *arg, const char *name);
struct tcb *sched_create_user(const char *name, uint64_t user_rip, uint64_t user_stack);
/* fork (5b slice 8): clone `parent` into a ready ring-3 child whose AS is
 * `child_cr3`, resuming at `entry`/`user_rsp` (RAX=0 via enter_user_mode). Copies
 * the cap table (cap_fork) and fd table (fd_clone). Returns the child, or 0 on
 * OOM (any partial child is destroyed). */
struct tcb *sched_create_user_clone(struct tcb *parent, uint64_t child_cr3,
                                    uint64_t entry, uint64_t user_rsp);
/* Unlink a never-run / reaped thread from the ready ring and free its kstack,
 * cap table, open files, and TCB. Not for the currently running thread. */
void        sched_destroy(struct tcb *t);
void        sched_tick(void);                                   /* from the timer IRQ      */
void        yield(void);                                        /* cooperative switch      */
void        sched_block(void);                                  /* block current; switch away */
void        sched_unblock(struct tcb *t);                       /* mark a blocked thread ready */
void        sched_exit(int status);                             /* zombie + status; wakes waiter */
void        sched_start_reaper(void);                           /* spawn the orphan-zombie reaper */

extern struct tcb *current_thread;
