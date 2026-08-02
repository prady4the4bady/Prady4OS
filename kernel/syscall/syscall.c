/* kernel/syscall/syscall.c — NSI dispatch table, handlers, and MSR setup. */
#include "syscall.h"
#include "console.h"
#include "cap.h"
#include "sched.h"
#include "uaccess.h"   /* validated user-pointer copy path (ADR-022); used by 5b syscalls */
#include "errno.h"
#include "sys_io.h"    /* SYS_READ / SYS_WRITE handlers (slice 3) */
#include "sys_file.h"  /* SYS_OPEN / SYS_CLOSE / SYS_FSTAT handlers (slice 4) */
#include "sys_proc.h"  /* SYS_LSEEK / SYS_GETCWD handlers (slice 5) */
#include "sys_mmap.h"  /* SYS_MMAP / SYS_MUNMAP handlers (slice 6) */
#include "sys_exec.h"  /* SYS_EXECVE handler (slice 7) */
#include "sys_fork.h"  /* SYS_FORK handler (slice 8) */
#include "sys_wait.h"  /* SYS_WAIT4 handler (slice 9) */
#include "pipe.h"      /* SYS_PIPE / SYS_DUP2 handlers (PROC-A) */
#include "epoll.h"     /* SYS_EPOLL_* handlers (PROC-B) */
#include "signal.h"    /* SYS_SIGACTION / _KILL / _SIGRETURN (PROC-C) */
#include "sys_io_uring.h" /* SYS_IO_URING_* handlers (PROC-E) */
#include "aether.h"       /* AETHER syscalls + per-agent rate limit (Layer 6) */
#include "percpu.h"       /* DDR-SMP-3a: %gs:0 probe from syscall context */

void sys_aether_register(void);   /* kernel/syscall/sys_aether.c */
void sys_socket_register(void);   /* kernel/syscall/sys_socket.c (ADR-027) */
void sys_fb_register(void);       /* kernel/syscall/sys_fb.c (DDR-702) */
void sys_input_register(void);    /* kernel/syscall/sys_input.c (DDR-703) */
void sys_surface_register(void);  /* kernel/syscall/sys_surface.c (DDR-706) */

#define MAX_SYSCALLS 128  /* NSI-v2 table size (ADR-022). Raised 80->128 in the
                           * DDR-823 audit: NSI 77-87 are already sequenced and
                           * 80+ would have registered into the void. 128 is
                           * headroom, not a prediction — syscall_register()
                           * now panics rather than dropping silently, so the
                           * next overflow announces itself at boot. */

static syscall_fn table[MAX_SYSCALLS];
/* syscall_kstack_top moved into the percpu area at [gs:16] (DDR-SMP-3b); the
 * user-register snapshot (rsp/rip/callee-saved/rflags) moved there too at
 * [gs:56..120] — per-CPU, written by syscall_entry.asm (ADR-031 cap-4). */

extern void syscall_entry(void);   /* arch/x86_64/syscall_entry.asm */

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_SFMASK 0xC0000084u

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* DDR-823 audit. This used to be a bare `if (num < MAX_SYSCALLS)` — an
 * out-of-range registration was SILENTLY DISCARDED. The syscall then returned
 * -ENOSYS at runtime, so the symptom appeared in a gate, far from the cause,
 * looking like a broken handler rather than a table that was never populated.
 *
 * That matters right now rather than hypothetically: NSI 77-87 are already
 * sequenced (ACC 77/78, AGS 79/80, agent-update 81, memory 82/83, checkpoint
 * 84/85, rewrite-approve 86, read-audit 87). With the old MAX_SYSCALLS of 80,
 * everything from 80 up would have registered into the void.
 *
 * Now it is loud and it is at boot, where a programming error belongs. Same
 * principle as ci-shard-check and ci-start-align-check: a silent drop that
 * looks like success is the failure mode this project keeps paying for. */
void syscall_register(unsigned num, syscall_fn fn) {
    if (num >= MAX_SYSCALLS) {
        kputs("PANIC: syscall_register() past MAX_SYSCALLS — raise it in "
              "kernel/syscall/syscall.c. NSI numbers are append-only, so this "
              "is a table-size bug, never a reason to reuse a number.\r\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    table[num] = fn;
}

long syscall_dispatch(long num, long a1, long a2, long a3, long a4) {
    if (num < 0 || num >= MAX_SYSCALLS || !table[num])
        return -ENOSYS;
    /* AETHER rate limit (ADR-026 D7): agent processes get 60 syscalls / 1 s; an
     * over-budget agent is cleanly killed here and never reaches the handler.
     * Non-agents (init, PRISM, kernel) are exempt, so existing gates are intact. */
    if (current_thread && current_thread->is_agent &&
        aether_rate_check(current_thread) < 0)
        sched_exit(137);                         /* never returns */
    return table[num](a1, a2, a3, a4);
}

/* --- handlers ------------------------------------------------------------- */

static long sys_putc(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    cap_t cap = (cap_t)a1;
    /* Mutating op: require a capability bound to the console with display rights. */
    if (!cap_authorize(current_thread->caps, cap, RES_DEVICE, CONSOLE_RES_ID, CAP_DISPLAY))
        return -1;
    kputc((char)a2);
    return 0;
}

static long sys_getpid(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    /* DDR-SMP-3a probe (once): a %gs:0 read from a syscall entered at ring 3 —
     * this only works when the SWAPGS discipline is balanced. */
    static int gs_checked;
    if (!gs_checked) {
        gs_checked = 1;
        struct percpu *pc = this_cpu();
        if (pc && pc->self == pc)
            kputs("[percpu] gs OK (syscall ctx)\r\n");
        else
            kputs("[percpu] gs FAIL (syscall ctx)\r\n");
        /* DDR-SMP-3b: current_thread resolves through the same percpu slot. */
        if (pc && pc->current && pc->current == current_thread)
            kputs("[percpu] current OK (syscall ctx)\r\n");
        else
            kputs("[percpu] current FAIL (syscall ctx)\r\n");
    }
    return (long)current_thread->pid;
}

static long sys_yield(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    yield();
    return 0;
}

static long sys_exit(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    kputs("[user] sys_exit(");
    kputdec((uint64_t)a1);
    kputs(") — thread terminating\r\n");
    sched_exit((int)a1);       /* zombie w/ status; does not return */
    return 0;
}

void syscall_init(void) {
    for (int i = 0; i < MAX_SYSCALLS; i++)
        table[i] = 0;
    syscall_register(SYS_PUTC, sys_putc);
    syscall_register(SYS_GETPID, sys_getpid);
    syscall_register(SYS_YIELD, sys_yield);
    syscall_register(SYS_EXIT, sys_exit);
    sys_io_register();                   /* SYS_READ / SYS_WRITE (slice 3) */
    sys_file_register();                 /* SYS_OPEN / SYS_CLOSE / SYS_FSTAT (slice 4) */
    sys_proc_register();                 /* SYS_LSEEK / SYS_GETCWD (slice 5) */
    sys_mmap_register();                  /* SYS_MMAP / SYS_MUNMAP (slice 6) */
    sys_exec_register();                  /* SYS_EXECVE (slice 7) */
    sys_fork_register();                  /* SYS_FORK (slice 8) */
    sys_wait_register();                  /* SYS_WAIT4 (slice 9) */
    pipe_register();                      /* SYS_PIPE / SYS_DUP2 (PROC-A) */
    epoll_register();                     /* SYS_EPOLL_* (PROC-B) */
    signal_register();                    /* SYS_SIGACTION / _KILL / _SIGRETURN (PROC-C) */
    sys_io_uring_register();              /* SYS_IO_URING_* (PROC-E) */
    sys_aether_register();                /* SYS_GET_MODE..SYS_SET_MEM_LIMIT (Layer 6) */
    sys_socket_register();                /* SYS_SOCK_* proxy sockets (ADR-027) */
    sys_fb_register();                    /* SYS_FB_* framebuffer surface (DDR-702) */
    sys_input_register();                 /* SYS_INPUT_POLL / SYS_MOUSE_POLL (DDR-703/705) */
    sys_surface_register();               /* SYS_SURFACE_* per-client surfaces (DDR-706) */

    syscall_init_ap();                    /* the BSP's MSRs — same helper as the APs */
}

/* ADR-031 cap-4: the SYSCALL machinery MSRs are PER-CPU — an AP without
 * EFER.SCE takes #UD on a user `syscall` instruction. The dispatch table above
 * is global (once, BSP); the MSRs are armed on every CPU that runs ring 3. */
void syscall_init_ap(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);            /* EFER.SCE */
    /* STAR: [47:32]=0x08 (SYSCALL CS, SS=+8=0x10); [63:48]=0x10 (SYSRET base:
     * SS=+8=0x18 user data, CS=+16=0x20 user code, RPL forced to 3). */
    wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);                         /* clear IF on kernel entry */
}
