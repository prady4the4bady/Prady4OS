# DDR-SMP-3c-cap-4 — user threads on APs (ADR-031 D5, capstone final)

> DDR before code. The last ADR-031 sub-slice: remove the BSP pin on ring-3
> threads. Everything else is already per-CPU (TSS.RSP0 cap-1, `%gs`
> current/kstack cap-3b, CR3/FPU/FS-base per-thread on switch-in, SWAPGS 3a,
> per-AP preemption cap-3). What remains is the SYSCALL entry snapshot, signal
> delivery on AP ticks, and the un-pin + proof.

## Decisions
- **D1 — the SYSCALL register snapshot moves into `struct percpu`.**
  `syscall_entry.asm` stashes user RSP/RIP + callee-saved regs + RFLAGS in
  GLOBALS (`syscall_user_*`) on every entry. Two CPUs in syscalls concurrently
  overwrite each other — a fork on CPU A could clone CPU B's register state.
  The snapshot becomes per-CPU: `struct percpu` gains a `usnap` block at fixed,
  static-asserted offsets; `syscall_entry` writes `[gs:...]` (kernel GS is
  active right after `swapgs` — no external base register needed, and the old
  "stash briefly to a global before the stack switch" trick becomes safe
  per-CPU). `sched_create_user_clone` reads `this_cpu()->usnap` — same-CPU by
  construction (it runs in the forking thread's syscall) — and its `cli` guard
  (kept from cap-2a for the globals) is now truly redundant but harmless; it
  still prevents a mid-read local preemption from confusing the copy, so keep.
- **D2 — signal delivery on AP timer returns.** `timer_tick`'s ring-3 signal
  delivery is BSP-only (cap-3 D4). An AP returning to a ring-3 thread must also
  deliver: the AP branch in `isr_dispatch` calls `signal_deliver(r)` when
  `(r->cs & 3) == 3`, after `sched_tick`. The global-time side-effects stay
  BSP-only.
- **D3 — un-pin.** `pickable()` drops the `is_user && !is_bsp` rejection.
  `user_launch`/`schedule()` already set this CPU's RSP0 + kstack_top on
  switch-in, so ring-3 entry/exit is per-CPU-correct on any CPU.
- **D4 — proof without printing under the scheduler lock.** `schedule()` sets a
  `g_user_on_ap` flag when it claims a user thread on a non-BSP CPU; the BSP
  FS-phase proof polls it and prints `[smp] user on AP OK` (no console I/O from
  inside the locked scheduler path). The FS-phase already spawns many user
  processes (HELLO, SYSTEST, FPU pairs, INIT, PRISM...), giving APs plenty to
  pick up; the proof nudges with `smp_resched_all()`.

- **D5 — the AP trampoline's machine state is NOT the BSP's (found by the
  gate).** First run: every user process on an AP died instantly — `#UD` at its
  first `syscall` instruction and RSVD-bit `#PF`s on NX pages. The trampoline
  sets only PAE+LME; kmain's per-CPU machine-state setup ran on the BSP alone.
  Ring 3 on an AP needs, per CPU: `cpu_enable_sse` (CR0.MP/EM + CR4.OSFXSR —
  musl uses XMM), `EFER.NXE` (`vmm_enable_nxe_ap`, reusing the BSP's CPUID
  probe — without it PTE bit 63 is *reserved* and every W^X page faults RSVD),
  and the SYSCALL MSRs (`syscall_init_ap`: EFER.SCE + STAR/LSTAR/SFMASK; the
  dispatch table stays global/one-time). All armed in `smp_ap_entry` before
  joining the scheduler.

## Gate
`smoke-smpuser` (`-smp 4`): `[smp] user on AP OK` (+ the existing user
sentinels still green — user programs must run CORRECTLY on APs, not just run).
61 gates. This completes ADR-031 and the ADR-030 staged migration: every CPU
schedules, preempts, and runs ring-3 processes.

## Non-goals
Per-CPU runqueues / affinity / load balancing; tickless idle; IRQ routing off
the BSP (DDR-714 stage C); multi-in-flight block I/O.
