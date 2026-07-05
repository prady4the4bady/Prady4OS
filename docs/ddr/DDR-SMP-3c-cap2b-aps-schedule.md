# DDR-SMP-3c-cap-2b — APs enter the scheduler (ADR-031 D2/D3, part 2)

> DDR before code. Builds on cap-2a's SMP-safe internals: the APs leave their
> mailbox park loop and run **kernel** threads from the shared ready ring. User
> threads stay BSP-pinned until cap-4 (per-CPU SYSCALL state). First slice where
> a ring thread actually executes on a non-BSP CPU — proven by a new gate.

## Decisions
- **D1 — per-CPU idle.** One idle per CPU, flagged `is_idle` with `on_cpu =
  home`. The BSP idle is a small static struct; AP idles are **`kmalloc`'d** in
  `sched_ap_enter` (a full `struct tcb` × `PERCPU_MAX` in BSS overruns the
  low-mem image cap — big tables come from the heap, never BSS). All idles live
  in the shared ring (so a CPU walking from its idle reaches the whole ring);
  `pickable()` lets a CPU pick only **its own** idle (`t == g_idle[cpu]`), never
  another's — but the owner MUST round-robin back to its idle, because the idle
  is also that CPU's main context (the BSP idle runs `sched_demo`). An AP's idle
  adopts the AP's boot stack: its `rsp` is first written by `context_switch`'s
  save when it first switches away, so no seeding is needed.
- **D1b — APs start before `sched_init`.** `smp_start_aps` runs before the
  scheduler exists, so `sched_ap_enter` first spins in a park loop (drain the
  mailbox — the boot job-dispatch test runs pre-`sched_init` — then `hlt`) until
  a `g_sched_ready` flag is released at the end of `sched_init`; only then does
  it build its idle and join. The acquire/release pair makes `g_idle[0]` + the
  ring visible before the AP reads them.
- **D2 — `sched_ap_enter()` replaces the park loop.** The AP initializes+links
  its idle under `g_sched_lock`, sets `current = its idle`, and runs the idle
  loop: drain the mailbox job (keeps `smp_run_on` / `smoke-smpjob` /
  `smoke-crosswake` working), `schedule()` (pick up a ready kernel thread), then
  `sti; hlt` until the next interrupt. A wake IPI (existing vector 49) or a
  timer breaks the `hlt`; the loop re-schedules. Cooperative — an AP only
  reschedules on block/yield/exit/idle until cap-3 adds timer preemption.
- **D3 — kernel threads that return must `schedule()`, not `hlt`.**
  `thread_trampoline` ended a finished thread with `for(;;) hlt`, relying on the
  timer to preempt it away. On the BSP that works; an AP (no preemption yet)
  would be **stuck** in a finished thread's `hlt`, never freeing the CPU. So a
  returning kernel thread now sets `THREAD_DONE` and `schedule()`s away
  (cooperative exit; DONE is unpickable so it never runs again). Strictly better
  on the BSP too.
- **D4 — user threads are BSP-only until cap-4.** Once APs schedule, they would
  otherwise pick READY *user* threads — but a ring-3 thread on an AP hits the
  **global** `syscall_user_*` snapshot and other not-yet-per-CPU SYSCALL entry
  state (a cap-4 item). So `pickable()` gains an `is_bsp` argument and rejects
  `is_user` threads on non-BSP CPUs. `is_bsp` is a new `struct percpu` byte set
  for the BSP in `percpu_init_early` (and carried through the migration copy in
  `percpu_init_bsp`), so it survives a roster-index remap.

## Proof / gate
`smoke-smpsched` (`-smp 4`): a BSP FS-phase probe (gated on `g_smp_have_aps`)
creates several READY **kernel** probe threads, `smp_resched_all()`s the APs,
and spins (not yielding, so the BSP does not run them) until a probe records a
non-BSP `cpu_idx` in a shared mask. Prints `[smp] sched cross-CPU OK` iff
`(mask & ~bsp) != 0`, else `... FAIL` (forbidden). This is the first proof a
ring thread ran on an AP. 59 gates total.

## Non-goals (→ cap-3 / cap-4)
Per-AP LAPIC-timer preemption (APs are cooperative here); user threads on APs +
per-CPU SYSCALL entry state; per-CPU runqueues / load balancing / affinity API.
