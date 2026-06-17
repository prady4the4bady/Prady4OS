/* kernel/syscall/syscall.h — NEXUS syscall interface (NSI), Phase 2e.
 *
 * 64-bit SYSCALL/SYSRET. User mode puts the syscall number in RAX and args in
 * RDI, RSI, RDX, R10 (SysV syscall ABI); the return value comes back in RAX.
 * Mutating syscalls are capability-aware (they validate a capability handle the
 * caller passes).
 */
#pragma once
#include <stdint.h>

/* Syscall numbers. */
#define SYS_PUTC   1   /* (cap, char)  -> write one char to the console     */
#define SYS_GETPID 2   /* ()           -> current pid                       */
#define SYS_YIELD  3   /* ()           -> yield the CPU                      */
#define SYS_EXIT   4   /* (code)       -> terminate the calling thread      */

#define CONSOLE_RES_ID 1ull   /* capability resource id for the console */

typedef long (*syscall_fn)(long a1, long a2, long a3, long a4);

void syscall_init(void);                       /* program MSRs + register table */
void syscall_register(unsigned num, syscall_fn fn);
long syscall_dispatch(long num, long a1, long a2, long a3, long a4);  /* from asm */

/* Kernel stack top for the current user thread's SYSCALL entry (set on switch
 * to a user thread). The asm entry stub reads it; a brief scratch slot too. */
extern uint64_t syscall_kstack_top;
extern uint64_t syscall_user_rsp;
