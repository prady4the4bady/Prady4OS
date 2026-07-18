# DDR-758 — syscall-fuzz gate: the kernel survives hostile ring-3 NSI calls

**Status:** proposed (pre-code)
**Layer:** user probe + gate. M1 kernel hardening 2/3.

## Problem

The syscall boundary is the primary ring-3→ring-0 attack surface, but nothing
stress-tests it. The two robustness properties that must hold for *every* input
are (a) an out-of-range / unregistered NSI number never dereferences the dispatch
table off-end, and (b) a valid syscall handed a wild pointer returns `-EFAULT`
via the uaccess fixup and never faults the kernel. Both are implemented
(`syscall_dispatch` bounds-checks `num < 0 || num >= MAX_SYSCALLS || !table[num]`;
copyin/copyout have a fault fixup) — but there is no gate proving they hold under
a flood of adversarial inputs.

## Decision

A freestanding **`syscallfuzz` probe** (deterministic — a fixed-seed 64-bit LCG,
so every run is identical and CI-reproducible) issues ~3000 hostile syscalls,
then prints `PRADYOS_FUZZ_OK`. Two phases, chosen per-iteration by an LCG bit:

- **Bad NSI number:** `num` out of range (negatives, and `[MAX_SYSCALLS,
  MAX_SYSCALLS+4000)`) with wild args. Must hit the dispatch bounds check and
  return `-ENOSYS` — the handler is never reached, so this is side-effect-free.
- **Wild pointer into a real syscall:** `num` drawn from an **allowlist** of
  read-only / query syscalls that take user pointers — `READ, WRITE, FSTAT,
  GETCWD, SET_TLS, AGENT_ROSTER, AGENT_METRICS, GETDENTS, GETPROCS, SYSINFO,
  TIME, DMESG, MEMINFO, SETNAME` — with each arg a wild pointer (NULL, kernel VA,
  non-canonical, unmapped user, `0x41414141`). Each must return an error
  (`-EFAULT`/`-EBADF`/`-EINVAL`) with the kernel intact.

**Allowlist, not denylist** (the safety-critical choice): only calls that cannot
self-terminate the probe, kill another process, move memory, spawn, or power the
machine off are fuzzed with live handlers. Destructive/side-effectful NSIs
(`EXIT, MMAP, MUNMAP, EXECVE, FORK, KILL, SIGRETURN, SET_MODE, SET_MEM_LIMIT,
SPAWN_AGENT, KILL_AGENT, POWEROFF, REBOOT, SURFACE_CREATE/RESIZE, SOCK_*`) are
never invoked with a live handler — their dispatch path is still covered by the
out-of-range phase's bounds check, just not their bodies. Wild pointers can't
fault the probe: the uaccess path returns `-EFAULT` to the caller rather than
delivering the fault, so the probe always runs to completion.

## Gate — `smoke-syscallfuzz` (new; 93 → 94)

`EXTRA_SENTINEL=PRADYOS_FUZZ_OK`, `FORBIDDEN_SENTINEL` covers the panic markers
already caught by `boot_test.sh` (`[panic]`/`KERNEL PANIC`). Two-sided witness:
the sentinel prints only after all 3000 calls return (kernel alive through every
one), and any kernel fault during the flood is a panic → boot-test FAIL. Runs in
the normal boot alongside the other probes.

## Non-goals

- Not a coverage-complete fuzzer: destructive-NSI *bodies* are out of scope here
  (they have their own gates — kill/759, exec, fork, surface lifecycle); this
  gate targets dispatch-bounds + uaccess robustness.
- No structural/grammar fuzzing of individual arg encodings, no coverage
  feedback — a fixed-seed flood, bounded and reproducible.
- No in-kernel fuzz harness (the existing `net_fuzz_test` covers the packet path;
  this is the syscall path from real ring 3).
