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

#define MAX_SYSCALLS 64   /* NSI-v2 table size (ADR-022) */

static syscall_fn table[MAX_SYSCALLS];
uint64_t syscall_kstack_top;
uint64_t syscall_user_rsp;

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

void syscall_register(unsigned num, syscall_fn fn) {
    if (num < MAX_SYSCALLS)
        table[num] = fn;
}

long syscall_dispatch(long num, long a1, long a2, long a3, long a4) {
    if (num < 0 || num >= MAX_SYSCALLS || !table[num])
        return -ENOSYS;
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
    sched_exit();              /* does not return */
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

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);            /* EFER.SCE */
    /* STAR: [47:32]=0x08 (SYSCALL CS, SS=+8=0x10); [63:48]=0x10 (SYSRET base:
     * SS=+8=0x18 user data, CS=+16=0x20 user code, RPL forced to 3). */
    wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);                         /* clear IF on kernel entry */
}
