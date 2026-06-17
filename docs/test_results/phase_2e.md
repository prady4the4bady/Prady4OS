# Phase 2e — Syscall Interface (NSI) + ring 3 — Test Results

## Slice: SYSCALL/SYSRET + TSS + ring-3 user thread

- **Date:** 2026-06-18
- **Decision:** ADR-012.
- **Files:** `arch/x86_64/cpu.asm` (GDT: user segs + TSS slot), `kernel/proc/tss.c`,
  `kernel/syscall/syscall.{c,h}`, `arch/x86_64/syscall_entry.asm`,
  `arch/x86_64/usermode.asm` (ring-3 entry + user blob), `kernel/proc/sched.c`
  (user threads), `kernel/mm/vmm.c` (user-walk fix), `kernel/main.c` (demo).

### Verified (QEMU)

```
NEXUS: TSS loaded, SYSCALL/SYSRET armed
NEXUS: ring-3 user thread created (will syscall from user mode)
UR:9
[user] sys_exit(0) — thread terminating
```

The user thread, running in **ring 3** (confirmed earlier by a fault dump
showing CS=0x23), executed:
- `SYS_PUTC('U'/'R'/':')` — capability-gated console writes (each required a
  capability bound to the console resource with CAP_DISPLAY) → `UR:`
- `SYS_GETPID` → returned 9; the value flowed back to ring 3 in RAX and the user
  printed it (`9`)
- `SYS_YIELD` → cooperative switch and resume
- `SYS_EXIT(0)` → clean thread termination

No faults; smoke PASS; warning-free `-Werror` build.

### Debugging notes (root-caused, not patched)

1. **Linker warning** `.bss not a multiple of alignment (64)` from the IPC ring's
   `_Alignas(64)`. Fixed by aligning the `.bss` output section to 64 in
   `kernel/kernel.ld` (cache-line alignment is intentional).
2. **#PF entering ring 3** (`CR2=0x40000000`, CS=0x23): the page walk to the user
   page was denied at `PML4[0]`, which the bootloader created without the user
   bit. Fixed by having `vmm_map` promote existing intermediate entries to
   user-walkable; the leaf still gates actual access, so kernel pages remain
   kernel-only.

### Not done yet

- W^X / NX on user pages (currently W+X); per-process address spaces / CR3.
- The broad POSIX/agent/sovereign syscall set (Phases 5–6).
- 3-lane NAS scheduler, APIC — still deferred.

**Layer 2 (NEXUS kernel core) is complete:** memory (PMM/heap/higher-half VMM),
interrupts + exceptions, preemptive scheduler, capabilities, IPC (sync/async/
broadcast), and syscalls + ring-3 user mode.
