# ADR-012: Syscalls (NSI) + ring-3 user mode

- **Status:** Accepted 2026-06-18
- **Phase:** 2e

## Context

The last Layer-2 core piece: the syscall interface. To be real (not a stub) it
needs an actual ring-3 context to call from. The user approved building full
SYSCALL/SYSRET plus a minimal ring-3 thread (TSS + user segments) that issues
capability-aware syscalls.

## Decision

- **GDT** (`arch/x86_64/cpu.asm`) extended with user data (0x18, DPL3), user
  code64 (0x20, DPL3), and a TSS descriptor (0x28), laid out for SYSRET
  (STAR base 0x10 → user SS 0x18, user CS 0x20). GDT moved to `.data` so the
  TSS descriptor can be patched at runtime.
- **TSS** (`kernel/proc/tss.c`): provides RSP0 (the ring-0 stack the CPU uses for
  interrupts taken in ring 3). RSP0 is set to the current user thread's kernel
  stack on entry to ring 3 and on every switch to a user thread.
- **SYSCALL/SYSRET** (`kernel/syscall/`): `syscall_init` sets EFER.SCE, STAR,
  LSTAR (= the asm entry), SFMASK (clears IF, so syscalls don't nest).
  `arch/x86_64/syscall_entry.asm` switches to the thread's kernel stack, saves
  the registers the ABI requires preserved, marshals args, calls
  `syscall_dispatch`, then SYSRETs. ABI: number in RAX; args RDI/RSI/RDX/R10;
  return in RAX; RCX/R11 clobbered.
- **Dispatch table** with `syscall_register`; syscalls: SYS_PUTC (cap-gated
  console write), SYS_GETPID, SYS_YIELD, SYS_EXIT.
- **Capability-aware:** the mutating syscall (SYS_PUTC) requires a capability
  bound to the console resource with CAP_DISPLAY; the user thread is granted
  exactly that one capability.
- **Ring-3 threads** (`kernel/proc/sched.c`): a TCB gains `is_user`, `pid`,
  `user_rip/stack/arg`. `sched_create_user` makes a kernel thread whose launch
  routine sets RSP0, then `enter_user_mode` (`arch/x86_64/usermode.asm`) IRETQs
  to ring 3. The user program is position-independent code copied into a
  user-mapped page; user code/stack are mapped with `vmm_map(..., VMM_USER)`.
- **VMM user-walk fix:** `vmm_map` now promotes *existing* intermediate page-table
  entries to user-walkable (the bootloader-made PML4[0] lacked the user bit);
  actual access is still gated by the leaf, so kernel pages stay kernel-only.

## Consequences / deferred

- One syscall stack per thread; SFMASK clears IF so no syscall nesting (no
  swapgs/per-CPU needed yet on single CPU).
- User code page is mapped W+X for now; W^X/NX hardening is a later slice.
- "200+ syscalls" is aspirational; the mechanism + 4 calls are in. POSIX/agent/
  sovereign syscalls come with userspace and AETHER (Phases 5–6).

## Verification

QEMU: the ring-3 thread prints `UR:` then its pid (`9`) via cap-gated SYS_PUTC,
calls SYS_GETPID (value returned to ring 3), SYS_YIELD, and SYS_EXIT (clean
termination). No faults; smoke PASS; warning-free `-Werror` build (the `.bss`
cache-line alignment warning was fixed in the linker script).

**This completes Layer 2 — the NEXUS kernel core.**
