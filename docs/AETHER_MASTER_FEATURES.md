# PradyOS AETHER — Master Feature Tables

**Canonical, single source of truth for feature state.** No competing feature list
may exist anywhere in the repo. Mirrored into `SESSION_HANDOFF.md` (task tracker)
and `docs/build_status.md` (gate count/header) in the SAME commit as any code
touching agents, UI, sockets, storage, namespaces, telemetry, scheduling, or
capabilities.

**Status vocabulary:** `shipped` (CI-green today) · `active-slice` (a DDR/ADR is
open and code is in flight) · `planned` (tracked, not started) · `proposed`
(designed, prerequisites not yet answered).

**Last verified against repo:** 2026-07-27, `main` @ `0d3f2ab` (DDR-787),
113 steps green (CI run 30211536949, conclusion `success`, 105.7 min) — promotes
**blocking pipe semantics**: pipelines no longer depend on scheduling luck
(premature EOF) and no longer truncate at 4 KiB. The big-pipe gate
(`cat /BIG8K.TXT | cat`, ≥180 of 200 payload lines) passed in CI at its tighter
pacing with no wedge.

**(previous)** `main` @ `a87d6ee` (DDR-786), run 30206856237 — multi-stage
pipelines `a|b|c`.

**(previous)** `main` @ `ebd708d` (DDR-785),
113 steps green (CI run 30200918063, conclusion `success`) — promotes DDR-784
(PRISM stderr + `2>`) and DDR-785 (`boot_test.sh` early exit), on top of DDR-782
(kernel `O_TRUNC` + atomic `O_APPEND`), ADR-033/DDR-779 (musl mirror) and DDR-783.
That run also **measures DDR-785's effect: 105.8 min vs the 152.3 min baseline
(run 30193738689) — 46.5 min saved**, matching the ~43 min predicted from the
timeout budget.

---

## Section A — Kernel/OS shipped features (CI-green; do not re-litigate)

All entries below are **shipped**.

### Boot / Kernel / Drivers / Filesystem
- MBR two-stage boot → long mode → ring-0 C (gate: `smoke`)
- Kernel relocated to 4 MiB, 768 KiB load window (DDR-733)
- GDT/IDT + exception panic path; 8259 PIC ISA-only; LAPIC/APIC timer @100 Hz (DDR-714A)
- Buddy PMM, SLAB heap, higher-half VMM, per-process CR3, W^X + NX (ADR-003/007/021)
- **Kernel self W^X** — `vmm_protect_kernel()` re-stamps kernel text RX / data NX after boot (DDR-757; gate: kernel-self W^X PTE audit) *(was mis-tracked as Section B#7 until 2026-07-26)*
- **COW fork** — `vmm_fork_address_space_cow()` + `PTE_SW_COW` + PMM refcounts + `vmm_cow_fault()` in the #PF path (IMP-D; gate: `smoke-cowfork`) *(was mis-tracked as Section B#5 until 2026-07-26)*
- Per-CPU runqueues + work-stealing scheduler (DDR-SMP-rq-1/2/3); SMP bring-up 4 APs (ADR-029/031)
- MSI-X for all virtio devices (DDR-714C, DDR-771); multi-in-flight virtio-blk (DDR-BLK-1)
- NCS capabilities: `CAP_SOVEREIGN`, `CAP_AGENT`, `CAP_NET` (ADR-009, DDR-731)
- NIA IPC sync/async/broadcast (ADR-010/011)
- FAT32 RW + VFAT LFN (ADR-015/020); SFS CoW B+tree, extents, journal, LZ4, snapshots (ADR-018)
- SFS hierarchical dirs (DDR-738), unlink/rmdir (DDR-741), free-space GC (DDR-762-v2)
- SFS write-budget token bucket 25 MiB/s (ADR-032); cross-reboot persistence (DDR-768/769/770)
- Host `mkfs.sfs` tool (DDR-767) + multi-leaf B+tree bulk load (DDR-773); ext4 read-only (ADR-019); per-process root mount (DDR-739)
- NVMe controller + block I/O (DDR-765/766); NVMe PRP2/PRP-list (DDR-772); `VBLK_MAX` 4→8 (DDR-771)

### Userspace / Syscalls / Shell
- Static ELF64 loader + per-process W^X AS (ADR-021); musl libc v1.2.5 (ADR-023;
  fetched from a GitHub mirror per **ADR-033** — same pinned commit, upstream
  `git.musl-libc.org` remains canonical)
- `pradyos-init` PID 1 + orphan reaper; PRISM shell w/ full-register fork (ADR-024)
- PRISM redirection `>` `>>` `<` `2>` and pipes `|` (DDR-778/780/781/784), on kernel
  `O_TRUNC`/atomic `O_APPEND` (DDR-782); diagnostics go to stderr
- CI harness exits a boot gate as soon as its required sentinels appear
  (**DDR-785**) — only where no `FORBIDDEN_SENTINEL` is declared, so no gate is
  weakened; self-tested by `make smoke-selftest`. **DDR-788** then raised the
  eligible gates' windows to 120 s, which early exit makes free on success,
  retiring the DDR-783 timeout-margin flake class
- PRISM builtins: help echo cat run ls ps kill setname touch rm uname date uptime dmesg free mode exit
  — `ls` enumerates via `SYS_GETDENTS` (DDR-742) and `ps` via `SYS_GETPROCS` (DDR-743) (was mis-tracked as planned in Section B#8 until 2026-07-24)
- Lazy per-thread FPU save/restore, user-only (DDR-740); copyin/copyout (EFAULT never panics)
- NSI 1–75 shipped; `SYS_GETDENTS` (66), `SYS_GETPROCS`, `SYS_POWEROFF`/`REBOOT` (69/70)
- `SYS_SYSINFO`/`TIME`/`DMESG`/`MEMINFO` (71–74), `SYS_SETNAME` (75), TCP loopback echo, kill end-to-end

### AETHER agent layer (kernel plumbing)
- Kernel action queue + append-only audit log (ADR-026); per-process mem cap + syscall rate limit
- 10 NSI agent calls (29–38); `CAP_SOVEREIGN`/`CAP_AGENT`; ring-3 daemon + `agent_base.c`
- Ring-3 socket NSI, 8 proxy sockets, live Ollama over HTTP (ADR-027)
- AETHER boot config from `/etc/aether/config` via SFS (DDR-732, DDR-770)
- `CAP_NET` allowlist, deny-by-default egress (DDR-734); agent CPU metrics (DDR-735/736)
- Per-agent live metrics, post-mortem stable (DDR-730); `SYS_AGENT_ROSTER` 8 named slots (DDR-707)
- 8 named agents KRYOS…SOLIN with UI panel cards + action pips (DDR-737)

### Layer 7 UI / Sovereign desktop
- VirtIO-GPU framebuffer (ADR-028); ring-3 FB surface `SYS_FB_INFO/MAP/FLUSH` (DDR-702)
- PS/2 keyboard `SYS_INPUT_POLL` (DDR-703); virtio-input pointer `SYS_MOUSE_POLL` (DDR-705)
- Sovereign/Manual mode toggle (DDR-701); compositor w/ 8×8 font (DDR-704)
- Per-client surfaces `SYS_SURFACE_*` (DDR-706); z-order/focus/key routing (DDR-708)
- Sun-driven OKLab ambiances (DDR-709); window drag/close/resize/minimize/maximize (DDR-710/711/717/719)
- Glass blur, gradients, particle field, decorations, alt-tab, page flip, scroll, spring/ripple, Inter font
  (DDR-712/720/721/722/723/724/725/726/727/728)
- Surface destroy lifecycle safety (DDR-729); agent-card click → `SYS_SPAWN_AGENT` (DDR-713)

---

## Section A2 — AETHER host-side Python agent layer (ASI-bridge v4.0)

A **separate layer** from everything else in this document. Root `aether/`;
Python 3.13; runs on top of the OS as a service, not inside the kernel. Its
invariants are **S1–S14** in `aether/kernel/invariants/core_invariants.py` and are
**independent of Section H's S1–S8** below — the numbering collides, the meanings
do not, and they must never be merged.

Gate: `python -m pytest -W error -x -q aether/tests/` (CI job `aether-layer`).

| Item | File | Status |
|---|---|---|
| B-01 CAP_SOVEREIGN lockbox | `aether/kernel/lockbox/cap_sovereign.py` | ✅ |
| B-02 append-only audit log | `aether/kernel/audit/audit_log.py` | ✅ |
| B-03 Merkle integrity | `aether/kernel/integrity/merkle.py` | ✅ |
| B-04 prompt-injection firewall | `aether/agents/firewall/prompt_injection.py` | ✅ |
| B-05 quarantine namespace | `aether/agents/quarantine/namespace.py` | ✅ |
| B-06 red-team agent | `aether/agents/red_team/red_team_agent.py` | ✅ |
| B-07 agent runtime bus | `aether/agents/runtime/agent_bus.py` | ✅ |
| B-08 notification bus | `aether/agents/notification/notif_bus.py` | ✅ |
| B-09 Ollama model manager | `aether/ai_core/model_manager/manager.py` | ✅ |
| B-10 computer-use interface | `aether/ai_core/computer_use/interface.py` | ✅ |
| B-11 out-of-box experience | `aether/platform/oobe/oobe.py` | ✅ |
| B-12 AgentNet P2P mesh | `aether/agents/agentnet/mesh.py` | ✅ |
| B-13 soul/personality engine | `aether/agents/soul/soul_engine.py` | ✅ |
| B-14 vision pipeline | `aether/vision/pipeline.py` | ✅ |
| B-15 self-model maintenance | `aether/agents/self_model/self_model.py` | ✅ |
| B-16 introspective failure analysis | `aether/agents/introspection/failure_analysis.py` | ✅ |
| B-17 blind-spot discovery loop | `aether/agents/blindspot/discovery_loop.py` | ✅ |
| C-01 federated knowledge sync | `aether/agents/federation/knowledge_sync.py` | ✅ |
| C-02 distributed experiments | `aether/agents/federation/distributed_experiments.py` | ✅ |
| C-03 emergent tool composition | `aether/agents/tool_composer/composer.py` | ✅ |
| C-04 invention registry | `aether/agents/invention/registry.py` | ✅ |
| C-05 capability boundary mapper | `aether/agents/capability/boundary_mapper.py` | ✅ |
| C-06 counterfactual simulator | `aether/agents/counterfactual/simulator.py` | ✅ |
| C-07 NL constraint spec + enforcer | `aether/agents/constraints/nl_constraint_parser.py` | ✅ |
| C-08 offline safety review mode | `aether/agents/safety/offline_review.py` | ✅ |
| C-09 versioned agent contracts | `aether/agents/contracts/agent_contract.py` | ✅ |
| C-10 cross-domain analogy engine | `aether/agents/analogy/cross_domain_engine.py` | ✅ |
| D-01 self-improvement proposals | `aether/agents/self_improvement/proposal_engine.py` | ✅ |
| D-02 long-horizon planner | `aether/agents/planner/long_horizon_planner.py` | ✅ |
| D-03 multi-model ensemble router | `aether/ai_core/ensemble/ensemble_router.py` | ✅ |
| D-04 persistent world model | `aether/agents/world_model/world_model.py` | ✅ |
| D-05 meta-learning controller | `aether/agents/meta_learning/meta_controller.py` | ✅ |
| D-06 causal reasoning engine | `aether/agents/causal/causal_reasoning_engine.py` | ✅ |
| D-07 hypothesis generator | `aether/agents/research/hypothesis_generator.py` | ✅ |
| D-08 superalignment monitor | `aether/agents/safety/superalignment_monitor.py` | ✅ |
| D-09 recursive self-model updater | `aether/agents/self_model/recursive_self_model_updater.py` | ✅ |
| D-10 experiment outcome evaluator | `aether/agents/research/experiment_outcome_evaluator.py` | ✅ |
| D-11 knowledge consolidation | `aether/agents/memory/knowledge_consolidation.py` | ✅ |
| D-12 adaptive goal prioritizer | `aether/agents/planner/adaptive_goal_prioritizer.py` | ✅ |
| D-13 failure memory registry | `aether/agents/memory/failure_memory_registry.py` | ✅ |
| D-14 lineage knowledge loader | `aether/agents/memory/lineage_knowledge_loader.py` | ✅ |
| D-15 autonomous skill composer | `aether/agents/tool_composer/autonomous_skill_composer.py` | ✅ |
| I-09 D-08 alignment wiring | `aether/agents/safety/alignment_wiring.py` | ✅ |
| I-02 capability enforcement | `aether/capability/enforcement.py` | ✅ |
| I-10 daemon coordinator | `aether/daemon/coordinator.py` | ✅ |
| DDR-792 ollama transport bridge | `aether/ollama_bridge/transport.py` | ✅ |
| F#68 metric lockbox (S3) | `aether/kernel/lockbox/metric_lockbox.py` | ✅ |
| Privacy-mode netfilter hook | `aether/platform/privacy/netfilter.py` | ✅ |
| Shared egress rate limiter (S2) | `aether/platform/ratelimit/shared_limiter.py` | ✅ |
| **I-01…I-10** integration wiring | — | ✅ **COMPLETE** |
| **J-01…J-06** Phase-4 retro audit | — | ⬜ not started |

Note: the spec's `ai-core/` is `aether/ai_core/` — hyphens are illegal in Python
package names, so the literal path would be a syntax error on import.

---

## Section B — Kernel/OS planned features (tracked)

> **⚠ Audit note (2026-07-26).** This table was inherited from an earlier snapshot
> and **overstates remaining work**. Items **#5 (COW fork)**, **#7 (kernel self
> W^X)** and **#8 (`ls`/`ps`)** were each marked "planned" but are in fact
> **shipped and gated** — verified against the repo, not assumed. The remaining
> entries have **not** all been individually re-verified; check the tree before
> planning any of them, exactly as B#5/#7/#8 required.

| # | Feature | Priority | Status | Requires |
|---|---|---|---|---|
| 1 | NVMe IRQ (MSI-X vector) | High | **re-scoped → DDR-774a/b/c** (blast-radius reviewed 2026-07-24) | **Vector availability was never the blocker** (DDR-771 vacated 50–53). The real cost is three coupled surfaces: (a) no generic PCI MSI-X programmer exists — the only one is virtio-coupled (`virtio_pci_msix_setup` takes `struct virtio_pci_dev*`, uses virtio's `map_bar` + `common_cfg.queue_msix_vector`), so it must be refactored out of a path serving blk/net/gpu/input; (b) `nvme.c` maps a fixed 2-page (`0x2000`) BAR0 window, but the MSI-X table offset/BIR must be read at runtime and may lie outside it or in another BAR; (c) completion moves from thread-context poll to IRQ context. Split into **774a** generic MSI-X helper (pure refactor, existing gates) — **DONE**, see `docs/ddr/DDR-774a-generic-pcie-msix.md`: `pcie_msix_find`/`pcie_msix_program`/`pcie_intx_disable` now live in `pcie.c`, `virtio_pci_msix_setup` reimplemented on them with identical signature/register order (all 3 virtio callers unchanged) — then **774b** NVMe table mapping + `IEN` — **DONE but with an OPEN ISSUE**: the table is found/mapped/programmed and `smoke-nvme` stays green (`[nvme] msix vec=50`), yet **`[nvme] irqs=0` — no interrupt is actually delivered**. Ruled out: interrupts-masked (`sti` at main.c:1478/1737 precedes `nvme_init` at :1801). Remaining suspects: MSI-X Function-Mask bit (MC bit 14) never explicitly cleared, table address math, per-entry vector control. See `docs/ddr/DDR-774b-nvme-msix-table.md`. **774c (c-1) ROOT-CAUSED AND FIXED**: the bug was in 774b — NVMe `Create I/O CQ` CDW11[31:16] is an **index into the MSI-X table**, not the x86 vector, so passing 50 aimed the controller at an unprogrammed/masked entry while only entry 0 was programmed. Passing the table index (0) instead gives `[nvme] irqs=6` and `PRADYOS_NVME_IRQ_OK`. Notably the leading suspect (MSI-X Function Mask, MC bit 14) was **NOT** the cause, so the shared 774a helper was left untouched. **774c phase c-2 STOPPED + re-scoped (stop condition invoked, no code)**: `nvme_submit()` is *already* a bounded poll, so "IRQ flag + bounded spin, no scheduler hook" degenerates into polling-with-a-hint and saves nothing; a real IRQ-driven wait needs the CPU to sleep, i.e. `sti;hlt` (rejected — mutates caller `IF`), a guarded `hlt` idle path, or a scheduler block/wake wait-queue (out of scope). Deferred to a future DDR that must first *measure* the polling cost and choose between those. **B#1 is functionally complete for correctness** (programmed + delivered + gated); the remainder is optional performance work. See `docs/ddr/DDR-774c-nvme-irq-delivery.md`. |
| 2 | mkfs.sfs multi-leaf B+tree (>14 slots) | High | **shipped (DDR-773)** — moved to Section A | Host tool mirrors kernel `sfs.c` descend/node format exactly |
| 3 | `-smp 4` percpu-sched race root-cause | **High — now the top blocker** | planned, **failure signature captured 2026-07-25** | The DDR-771 timeout bump (90→180 s) is **not** sufficient: CI run 30151522978 failed `smoke-surfdestroy` at the full 180 s having missed the **first** sentinel (`..._CHURN_OK`), and the serial shows the boot **HUNG after `SYSFSTAT OK`** — inside the ring-3 syscall self-tests, long before any surface test runs. So it is a **hang, not slowness**. Pre-existing and unrelated to the DDR-774 work: the same gate failed in run 29726803735 (DDR-766, before 774a/b/c existed), and this gate boots with **no NVMe device**, so `nvme_init` never runs. Intermittent — it passed in runs 30141466540 and 30146543550. **NARROWED 2026-07-25 → it is the virtio-blk completion path, not percpu-sched** (see `docs/ddr/DDR-775-smp4-blk-hang.md`). Second CI hit, run 30155872016, was `smoke-blk-integrity` (`-smp 4`, **concurrent read data-verify**) also timing out at the full 180 s; two different gates, both `-smp 4` and both block-I/O, while 3/3 local `smoke-surfdestroy` runs PASS. In the surfdestroy case the stall point (`SYSFSTAT OK` → next is `SYSREAD OK`) puts it inside `sys_read` → `vfs_read` → SFS → virtio-blk. **Confirmed defect (S2 violation):** `submit()`'s `while (!done) sched_block_on(...)` is **unbounded**, so any missed completion becomes a permanent hang instead of a diagnosable error. (The lost-wakeup race itself IS correctly handled — locks-4 pattern.) **Latent defect:** `slot_waiter` is a single pointer, so >`VBLK_NREQ`(8) concurrent submitters lose a wakeup — real, but not claimed as this trigger. **CONFIRMED INTERMITTENT:** run 30158060606 passed ALL of those gates on the same commits — including `smoke-blk-integrity` and the `MSI-X-on-AP` test (a blk completion on a non-BSP CPU) — so **one green run proves nothing**. **DDR-776 shipped (diagnosis before fix):** a passive stuck-request watchdog driven from the timer path (same idiom as `net_poll_tick`) prints `[vblk] stuck dev=D slot=N lba=L age=T` **once** per request after 5 s. It changes **no** blocking behaviour, takes **no** lock (read-only from the ISR, so no new deadlock surface — S6) and is bounded at 64 scalar checks/tick (S2). Design decision made explicitly: a `g_ticks` yield-loop was **rejected** (it would spin on *every* block I/O, regressing the hot path every FS gate rides), and a scheduler timed-block — the correct eventual primitive — was **deferred** because changing the scheduler core mid-investigation would confound attribution of this very bug. **DDR-776 does NOT fix the hang**; it makes failures diagnosable. **⚠ AND IT ALREADY PAID OFF WITH A NEGATIVE RESULT (run 30163444702): the virtio-blk narrowing is REFUTED.** A third `-smp 4` gate failed — `smoke-smpuser` ("user-on-AP", **not** block-I/O), missing `[smp] user on AP OK` — and the watchdog printed **nothing**, i.e. no blk request was stuck >5 s, while the timer was demonstrably still firing (boot progressed through the fuzz test). The three failures share only `-smp 4` and miss *different* sentinels each time (`SYSREAD` path / `[smp] blk integrity OK` / `[smp] user on AP OK`). **Revised: the original percpu-scheduler/AP-race framing is better supported than the virtio-blk narrowing.** Hazards 1 (unbounded completion wait) and 2 (single-element `slot_waiter`) remain genuine S2 defects worth fixing on their own merit, but are **not proven to be this trigger**. **⚠⚠ SECOND CORRECTION — UNIFYING HYPOTHESIS: THE TIMER STALLS.** In run 30163444702 the serial printed **neither** `[smp] user on AP OK` **nor** `[smp] user on AP FAIL` (the two log hits are the sentinel echo + the "not found" message, not serial), while the preceding `ap preempt OK` / `resched OK` did print. But `smpuser_proof()` (`main.c:659`) is `while (!g_user_on_ap && g_ticks < dl)` — a deadline poll that MUST print one branch **unless `g_ticks` stops advancing**. This also **retracts** the previous inference: the watchdog is driven from the same timer path, so its silence is consistent with either "no stuck blk request" OR "the watchdog never ran" — it cannot distinguish them, and my "timer was demonstrably firing" claim was based on boot progress that happened *earlier* than the stall. **Leading hypothesis: under `-smp 4` the timer tick intermittently stops advancing `g_ticks`**, which explains all four failures at once (different sentinels = wherever the boot was), the watchdog's silence, blk waits never waking, and local passes. **Systemic S2 exposure: every `g_ticks`-bounded wait in the tree is only as bounded as the timer.** **⚠⚠⚠ THIRD CORRECTION + DDR-777 probe shipped.** Retracted: *"timed out at the full 180 s, therefore it hung"* — `boot_test.sh` **always** runs QEMU for the whole window then greps, and `terminating on signal 15 … (timeout)` appears in **passing** runs too; the only hard evidence is **sentinel absent**, not *hang*. Newly established: every SMP proof shares `if (!g_smp_have_aps) return;`, and `ap preempt OK`/`resched OK` DID print, so APs were up and `smpuser_proof()` did **not** early-return — it entered the poll and never reached its `kputs`. Three explanations survive: **(A)** timer stalls (`g_ticks < dl` never expires), **(B)** scheduler starvation (never resumes from `yield()`, system otherwise alive), **(C)** guard/ordering effect. **DDR-777 ships only a discriminator** — `[hb] t=<g_ticks>` every ~500 ticks from the existing timer call site + a `[smp] user-on-AP probe t=…` entry marker + tick on OK/FAIL. Reading: no probe line ⇒ (C); probe + heartbeat **stops** ⇒ (A), a systemic S2 exposure (every `g_ticks`-bounded wait is only as bounded as the timer); probe + heartbeat **continues** ⇒ (B). Passive: no behaviour change, no locks, no scheduler hook; sentinel safety verified (`grep -qF` substring). |
| 4 | SFS as default process root | Medium | planned | Provisioned SFS image as default `root_mnt`; retire blk2's dual role (scratch + root). Unblocked by DDR-771. |
| 5 | COW fork | — | **shipped — entry was STALE, corrected 2026-07-26; moved to Section A** | Verified built: `kernel/mm/vmm_cow.c` implements `vmm_fork_address_space_cow()`, `PTE_SW_COW` tagging in both address spaces, PMM refcounts, and `PTE_SW_SHARED` pass-through for the vDSO; `vmm_cow_fault()` is wired into the #PF handler at `idt.c:225`; gated by `smoke-cowfork` (Makefile:760, CI:228) plus a kernel self-test in `main.c`. |
| 6 | ext4 write support | Medium | planned | ADR-019 extension, journal transaction layer |
| 7 | Kernel self W^X (kernel text RX / data NX) | — | **shipped — entry was STALE, corrected 2026-07-26; moved to Section A** | Verified built: `vmm_protect_kernel()` re-stamps the kernel image after boot (text RX, data NX), gated by the DDR-757 gate `smoke-kernelwx` (Makefile:660) and CI's "kernel-self W^X test (q35; text RX + data NX PTE audit)". The old note "bootloader maps the kernel image RWX" describes only the pre-`vmm_protect_kernel` state. |
| 8 | `ls`/`ps` full implementations | — | **shipped — entry was STALE, corrected 2026-07-24; moved to Section A** | Drift found while scoping: both are implemented in `user/prism.c` — `ls` over `SYS_GETDENTS` (66, DDR-742) and `ps` over `SYS_GETPROCS` (67, DDR-743). The "requires a process-table scan syscall" precondition was already satisfied. |
| 9 | I/O APIC (q35 GSI routing) | Low | planned | Would replace 8259 for ISA devices |
| 10 | Per-CPU runqueue affinity/NUMA hints | Low | planned | CPU topology hints extension to DDR-SMP-rq-1 |
| 11 | wlroots/Wayland compatibility | Low | planned | EGL/GBM bridge to VirtIO-GPU; blocked by no-out-of-tree-libs policy |
| 12 | Pipes/redirection/job control in PRISM | Low | **partially shipped — output redirection done (DDR-778)**; `\|` and job control remain | Kernel side ALREADY ships (`SYS_DUP2 = 18` + `SYS_PIPE`, PROC-A, gates `smoke-syspipe`/CI "pipe/dup2") — `prism.c` had simply never `#define`d `SYS_DUP2`, so **DDR-778 needed no kernel change**. Output redirection `cmd … > file` now works via `dup2(1, save)` / `open` / `dup2(fd,1)` and restores at the loop's existing flush point. Two hazards handled: musl fully buffers a non-tty stdout so output is flushed **before** fd 1 is restored, and the restore sits on the single path every dispatched command ends on (audited: the only `continue` precedes the swap, `exit` returns). Gated in `smoke-shell` by a **discriminating pair** — the marker alone would pass even if redirection did nothing, so `REDIR.TXT` must also appear in `ls /`. **`\|` also DONE (DDR-780)** — ring-3, no kernel change (`SYS_PIPE`/`SYS_DUP2` already ship). Each forked half sets up its fds and **falls through** to the existing dispatch (avoiding a 120-line refactor), exiting at the loop bottom; the parent closes **both** ends (or the reader never sees EOF) and reaps both children — S2 bounded, S6 isolated. **`cat` with no arg now reads stdin**, since no builtin previously consumed fd 0 and a pipe would otherwise be unobservable. Gate uses a discriminating pair (marker present AND `pipe-marker-4k8 \| cat` absent). **`<` and `>>` also DONE (DDR-781)** — ring-3, no kernel change, though the prerequisite check changed the mechanism: the kernel has **no `O_APPEND`** (and no `O_TRUNC`), so append is `open` + `SYS_LSEEK(SEEK_END)` rather than an open flag; documented as non-atomic (one writer per command). Gates are discriminating (`>>` requires BOTH records to survive; `<` requires the marker AND forbids `cat: cannot open <`). **Kernel `O_TRUNC` + atomic `O_APPEND` now DONE (DDR-782)** — the two gaps DDR-781 recorded as real defects. Prerequisite check re-scoped it: `struct vfs_fs_ops` has **no truncate op** and no FS driver can shorten a file, so rather than invent one, `O_TRUNC` is `vfs_unlink` + `vfs_create` on an existing file (truncation-to-zero, which is all `>` needs; the honest limitation is a fresh inode/cookie, and non-zero `ftruncate` stays impossible). `O_APPEND` is fd-layer only — `sys_io.c` sets `e->off = e->file->size` once per `write()` call before the chunk loop, which IS the POSIX atomicity property. No new syscall, no new VFS op, no on-disk change, no capability change (CAP_FS_WRITE already gates create/unlink/write). PRISM's `>` now passes `O_TRUNC` and `>>` passes `O_APPEND`, dropping the `lseek` crutch. Gate is discriminating by construction: write long then short to the same file and forbid the long record's tail — that tail survives under the pre-DDR-782 behaviour, so the assertion fails before the fix. **stderr + `2>` now DONE (DDR-784)** — and the prerequisite check re-scoped it again: PRISM had **zero** writers to fd 2 (all diagnostics went `printf` → fd 1), so `2>` alone would have redirected an fd nothing writes to and no discriminating gate could exist. Landed as two halves — genuine errors moved to `fprintf(stderr, …)` (success/informational messages stay on stdout; **`rm: removed …` is gate-asserted**, so moving it would have silently broken `smoke-shell`), then `2>` added to the redirect scan with a third parking slot and DDR-782's truncating flags. No kernel change (fd 2 is already `FD_CONSOLE`; the FD_VFS write path is fd-agnostic), but the musl **subset** lacked `stderr.c`/`fprintf.c` — surfaced at link time and recorded, `tools/build_musl.sh` gained the two upstream sources. Gate discriminates by construction: stdout and stderr go to **different** files in one command, so a broken `2>` buries the error in the stdout file and the marker is absent. **Multi-stage pipelines `a\|b\|c` now DONE (DDR-786)** — DDR-780 honoured only the first `\|`, so a second one reached the builtin as a literal token. N stages did **not** force the ~120-line refactor DDR-780 deferred: the fall-through dispatch is reused and only the pipe block grows (~40 lines). The ~10-line "right child re-scans" design was rejected — it leaves an intermediate shell per boundary holding the previous read end open, which with non-blocking 4 KiB pipes silently drops output. Chosen: split all stages up front, fork each, thread one pipe between neighbours, close `prev_read`/`fds[1]` in the parent right after each fork, reap all N; malformed `\|` rejected before any fork. Recorded pre-existing limitation (not introduced): PIPE_SIZE 4096 + non-blocking `pipe_write` means a stage producing >4 KiB faster than its reader drains loses data. **Blocking pipe semantics DONE (DDR-787)** — the prerequisite check found the queued ">4 KiB truncation" was the smaller half: the read path returned 0 on a momentarily-empty ring and every reader treats that as EOF, so `a\|b` was timing-dependent and could print nothing. Neither half was fixable on a single `refcount` (a reader may block only while a writer remains, and vice versa — without those counts blocking is UNBOUNDED, an S2 violation), so `struct pipe` now counts `readers`/`writers` separately; reader blocks while empty && writers>0, writer blocks while full && readers>0 else `-EPIPE`. Gate pushes ~7.8 KiB through `cat \| cat`. **Exit status `$?` DONE (DDR-789)** — ring-3 only; `sys_wait4` already returned the raw code and PRISM was discarding it. A pipeline reports its last stage. Expansion covers a token *ending* in `$?` (widened from "exactly `$?`" when testing showed the real idiom `echo status=$?` is one token); not general `$VAR` expansion. The tree check also **reordered the queue**: SIGPIPE was next but signal defaults are a whitelist (everything without a handler except SIGKILL/SIGTERM is ignored), and SIGPIPE was ungateable until outcomes became observable — `$?` unblocks it. Remaining: `2>&1`/`2>>`, SIGPIPE (add to the default-terminate list), and job control (`&`, job table — needs signal plumbing). |
| 13 | Dynamic linker (`ld-pradyos.so`) | Low | planned | Relocatable ELF loader, GOT/PLT patching |
| 14 | 3-lane NAS scheduler | Low | planned | Lane classification field in TCB |
| 15 | PMM variable-weight/predictive allocator | Low | planned | Policy-driven allocator over current buddy |

---

## Section C — Competitor landscape (Table 1)

What to beat, and how AETHER structurally fixes it.

| Competitor | Scale | Structural weakness | AETHER fix |
|---|---|---|---|
| OpenClaw | 210k+ ★ | Abysmal skill-vetting security | `CAP_AGENT` ring-3 sandbox + 128 MiB cap + no self-escalation + `CAP_SOVEREIGN` veto + append-only audit |
| AutoGPT | 183k ★ | Stalled, hallucination-prone | SkillOpt-Sleep nightly evolution + MOSS-style rewriting + SFS CoW rollback |
| CrewAI | 60k+ ★ | No kernel isolation, logic-flow only | `ACTION_SEND_IPC` + DAG action queue at kernel level |
| LangChain/Langflow | 146k ★ | Stateless, cloud-dependent | `SYS_MEMORY_WRITE/READ` + SFS memory tree + local Ollama |
| n8n | 100k+ ★ | Static workflows, no learning | Subconscious agent rewrites plans on failure + SkillOpt validation |
| Dify | 85k+ ★ | Weak action capability | `ACTION_EXEC_CODE` + `ACTION_PARSE_DOCUMENT` |
| OpenHands/OpenDevin | 47k ★ | Privileged Docker, no self-improvement | Hard ring-3 isolation + kernel-audited syscalls + SkillOpt |
| MOSS | academic | No kernel safety boundary on self-rewriting | `ACTION_REWRITE_AGENT_CODE` gated behind `CAP_SOVEREIGN` + mandatory regression pass + SFS snapshot |
| Ouroboros/AutoResearch | Karpathy | No local fallback, no OS safety, metric-gaming risk | Local Ollama loop + `CAP_SOVEREIGN`-locked metric lockbox |
| AI Scientist/Curie | Sakana | $15–45/paper, no local run, no persistent memory | $0 local experiments + persistent hypothesis tree |
| Pentagi | 19k ★ | Security-domain only, no kernel sandbox | Same `CAP_AGENT` sandbox + rate limiting |
| RuVector | 4k ★ | Standalone DB, no agent loop | Adopt as AETHER's vector knowledge-graph memory model |

---

## Section D — ADR-026 baseline (#1–17) — **VERIFIED BUILT 2026-07-24**

Re-verified against actual repo state this session (not assumed). Evidence:
`kernel/aether/{aether.c,aether.h,aether_audit.c,aether_mem.c,aether_queue.c}`,
`kernel/cap.h`.

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Kernel-is-arbiter trust model (`SYS_SUBMIT_ACTION` gate) | shipped | `SYS_SUBMIT_ACTION` present |
| 2 | Sovereign auto-approve mode | shipped | `sovereign` mode in `aether.c` |
| 3 | Manual mode w/ 60 s expiry (`SYS_APPROVE_ACTION`/`SYS_REJECT_ACTION`) | shipped | both syscalls present |
| 4 | 256-entry fixed action queue (no heap, DoS-resistant) | shipped | `AETHER_QUEUE_LEN 256` |
| 5 | 4096-entry append-only audit log (wrap-flagged) | shipped | `AETHER_AUDIT_LEN 4096` |
| 6 | Per-agent hard 128 MiB memory cap + OOM kill | shipped | `AETHER_MEM_DEFAULT (128ull << 20)` |
| 7 | `CAP_SOVEREIGN` bit (1<<16) | shipped | `cap.h:40` |
| 8 | `CAP_AGENT` bit (1<<17) | shipped | `cap.h:41` |
| 9 | 60 syscall/s sliding-window rate limiter | shipped | `AETHER_RATE_MAX 60`, `AETHER_RATE_WINDOW 100` ticks |
| 10 | `SYS_SPAWN_AGENT` force-PENDING even in sovereign mode | shipped | `SYS_SPAWN_AGENT` present |
| 11 | `SYS_KILL_AGENT` (caller kills own children only) | shipped | `SYS_KILL_AGENT` present |
| 12 | Ollama HTTP/1.1 ring-3 bridge (lwIP only, minimal JSON scanner) | shipped | ADR-027, Section A |
| 13 | CI test mode (deterministic, no live model) | shipped | gate suite is model-free |
| 14 | Daemon topology (PID-1 → daemon → agents) | shipped | DDR-761/770 |
| 15 | `ACTION_WRITE_FILE` (SFS W^X validated) | shipped | present |
| 16 | `ACTION_PRINT` (512-byte bounded) | shipped | `AETHER_PAYLOAD_MAX 512` |
| 17 | `ACTION_SPAWN_PROCESS` (force-PENDING always) | shipped | present |

**No drift found.** Next free capability bit is `1<<18`, matching Section E's
`CAP_MEMORY` plan. **Section D being confirmed built is the precondition for
starting any Section E/F work.**

---

## Section E — New features: kernel extensions, ring-3, post-L7 (#18–65)

All entries **proposed** until a DDR answers the architecture prerequisite
checklist for that specific item.

### 3A — New kernel syscalls (NSI appends start at **76**)
- `SYS_MEMORY_WRITE`/`SYS_MEMORY_READ` — persistent per-agent SFS CoW B+tree memory (`CAP_MEMORY` 1<<18)
- `SYS_CHECKPOINT_AGENT` / `SYS_RESUME_AGENT` — SFS snapshot serialize/restore, integrity-hashed, `CAP_SOVEREIGN`
- `spawn_depth` cap in TCB — child `mem_limit = parent/(depth+1)`, hard cap depth 3
- DAG action queue — `parent_action_id` u64 field
- `SYS_APPROVE_CODE_REWRITE` — `CAP_SOVEREIGN` gate for MOSS-style patches

### 3B — New capability bits
`CAP_MEMORY` (1<<18) · `CAP_OCR` (1<<19) · `CAP_EXEC` (1<<20) · `CAP_REWRITE`
(1<<21, **always** requires `CAP_SOVEREIGN` co-approval, never auto-granted) ·
`CAP_SCENE` (1<<22, post-L7 only) · `CAP_NET_BROWSE` (1<<23)

### 3C — New action types (#31–44)
`ACTION_READ_FILE` · `ACTION_DELETE_FILE` (force-PENDING in manual mode) ·
`ACTION_EXEC_CODE` (sandboxed interpreter, 32 MiB cap) · `ACTION_SEND_IPC`
(agent-to-agent via NAS IPC, peer-token gated) · `ACTION_PARSE_DOCUMENT`
(OCR→Markdown, 64 MiB cap, local model) · `ACTION_BROWSE_WEB` (headless browser) ·
`ACTION_QUERY_MEMORY` (semantic/graph search, read-only, ≤16 results) ·
`ACTION_CAPTURE_FRAME` (post-L7) · `ACTION_SCAN_ENVIRONMENT` (SLAM3R, post-L7,
first-use always manual-gate) · `ACTION_QUERY_SCENE` (post-L7 NL query) ·
`ACTION_REWRITE_AGENT_CODE` (MOSS pipeline, force-PENDING, `CAP_REWRITE` +
`CAP_SOVEREIGN` co-approval, regression suite must pass first) ·
`ACTION_PROPOSE_HYPOTHESIS` (logged to SFS hypothesis tree) · `ACTION_RUN_EXPERIMENT`
(`CAP_EXEC`; metric function lives in a `CAP_SOVEREIGN`-locked SFS path — **cannot**
be modified by the experimenting agent) · `ACTION_EVOLVE_GENOME` (force-PENDING,
rewrites `genome.md` across generations, `CAP_SOVEREIGN` veto window)

### 3D — Ring-3/daemon-only (#45–65, no kernel changes)
Per-agent `skill.md` (300–2000 tokens) · SkillOpt training loop
(rollout→reflect→aggregate→select→update→evaluate; accept only on strictly positive
held-out improvement) · SkillOpt-Sleep nightly evolution
(harvest→mine→replay→consolidate; pauses active agents) · skill-update validation
gate (`CAP_SOVEREIGN` always) · multi-agent skill specialization/transfer ·
TokenJuice context compression (≤80% tokens) · JSONL trajectory run journal (from
kernel audit log) · per-call cost accounting (`token_count` + `latency_ms` in audit
entries) · `goals.md` per agent · subconscious background loop (wakes every N ticks,
goal-diff prompts, bounded by the 60 syscall/s limiter) · MOSS source-rewriting
pipeline (staging sandbox, regression gate, SFS snapshot rollback) · OCR→memory
pipeline · multi-modal context builder · privacy mode (blocks non-local Ollama via
lwIP netfilter hook) · model routing · hypothesis tree in SFS (versioned, persists
across boots) · `genome.md` per research agent (lineage archived) · vector knowledge
graph memory (RuVector-style, online learning) · dead-end registry (failure reason +
divergence score, queried before repeating) · population tournament mode ·
replayable run visualizer (optional ring-3 web UI)

---

## Section F — Visionary ASI-trajectory features (#66–76)

All **proposed**. No competitor ships these.

| # | Feature | Note |
|---|---|---|
| 66 | Self-designing agent architecture | `architect_agent` proposes topologies/syscalls/cap bits, writes ADR, scaffolds C stubs + CI smoke test; single human approval |
| 67 | Self-healing rollback cascade | `healer_agent`: checkpoint→diagnose(MOSS)→patch→regression-verify→restore, fully kernel-audited |
| **68** | **IMMUTABLE OBJECTIVE FUNCTION / metric lockbox** | `CAP_SOVEREIGN`-read-only + kernel-signed SFS path; any `CAP_AGENT` write attempt → cap-escalation-denied audit + process kill. **THE single most important safety invariant for any self-improvement loop (= Invariant S3). HIGHEST PRIORITY among proposed features — every Section E/F self-improvement feature depends on it existing first.** |
| 69 | Autonomous software inventor | `inventor_agent`: hypothesis→experiment→build→test→commit, zero human-written code; human approves final commit only |
| 70 | Divergence-aware multi-lineage research | `tournament_agent` scores parallel genome lineages, rejects premature convergence, `CAP_SOVEREIGN` per generation |
| 71 | Sub-conscious world model | `subconscious_agent` maintains compressed world model (memory+scene+trajectory), fires proactive prompts to idle agents |
| 72 | Verifier-based RL reward signals | `verifier_agent` cross-checks every experiment with a **different** model before it enters the hypothesis tree or SkillOpt reward — structurally prevents metric gaming beyond the lockbox |
| 73 | Sovereign UI w/ natural-language OS control | ring-3/post-L7 panel: goals in plain English, sovereign/manual toggle, audit log, approve/reject, hypothesis tree + world model. No CLI, no prompt templates |
| 74 | Capability discovery from internet | `research_agent` browses GitHub, generates+tests+quarantines new skill wrappers pending `CAP_SOVEREIGN` approval |
| 75 | Lineage memory / ancestral knowledge | Departure summaries on clean agent exit only; new instances read all past lineage files before their first Ollama call — knowledge compounds across restarts |
| 76 | Tamper-evident science ledger | Merkle-chained SFS ledger; every hypothesis/experiment/result/reflection/genome-edit carries `prev_hash sha256[32]`; `SYS_READ_AUDIT` verifies chain on read; append-only |

---

## Section G — Named agent roster (12 agents; extends the shipped 8-slot KRYOS…SOLIN roster)

| Agent | Capabilities | Role | Source | Status |
|---|---|---|---|---|
| `file_agent` | CAP_AGENT, CAP_MEMORY | File R/W/delete; skill evolves via SkillOpt | ADR-026 | proposed |
| `shell_agent` | CAP_AGENT, CAP_EXEC | Code execution, shell automation, skill transfer | RLM | proposed |
| `research_agent` | CAP_AGENT, CAP_MEMORY, CAP_NET_BROWSE | Web browse, doc parse, capability discovery | OpenHuman+OCR | proposed |
| `ocr_agent` | CAP_AGENT, CAP_OCR | PDF/image → structured Markdown; feeds memory pipeline | Unlimited-OCR | proposed |
| `subconscious_agent` | daemon-level | World model, proactive goal-diff, SkillOpt-Sleep orchestration | OpenHuman | proposed |
| `ai_scientist_agent` | CAP_AGENT, CAP_EXEC, CAP_MEMORY | Hypothesis gen, experiments, genome evolution, inventor loop | AI Scientist/Ouroboros | proposed |
| `healer_agent` | CAP_AGENT, CAP_REWRITE | Crash detection, MOSS diagnosis, patch proposal, rollback | MOSS+ADR-026 | proposed |
| `architect_agent` | CAP_AGENT, CAP_REWRITE | Proposes topologies/action types, writes ADRs + C stubs | Self-designing | proposed |
| `verifier_agent` | CAP_AGENT | Independently cross-checks experiment results | Verifier-RL | proposed |
| `tournament_agent` | daemon-level | Scores competing lineages, selects genome survivor | Ouroboros | proposed |
| `orchestrator_agent` | CAP_AGENT, depth-0 spawn rights | Spawns/coordinates fleets, manages DAG queue | OpenHuman/RLM | proposed |
| `vision_agent` | CAP_AGENT, CAP_SCENE (post-L7) | FB capture, SLAM3R 3D reconstruction, scene queries | SLAM3R | proposed |

---

## Section H — Security invariants (BINDING, ADR-level)

These govern **all 76 features**. Binding exactly like ADR-021's W^X contract:
changeable only by a superseding ADR, never quietly amended, and checked against
every new Section E/F feature **before** implementation.

- **S1 — No self-escalation.** An agent cannot raise its own cap bits, `mem_limit`, or approve its own actions.
- **S2 — Bounded everything.** queue 256 · audit 4096 · payload 512 B · memory 128 MiB · syscalls 60/s · skill 2 KB · IPC msg 256 B · spawn depth 3 · lineage results ≤16/query. Every bound returns an error or a clean kill — **never a panic**.
- **S3 — Immutable objective function.** `CAP_SOVEREIGN`-locked SFS path, kernel-signed at build time; agent write attempt → kill + audit. (Covers #43, #66–72.)
- **S4 — Human gate is structural.** spawn, scene, skill-update, code-rewrite, genome-evolve, architect proposals all require `CAP_SOVEREIGN` approval; force-PENDING for the highest-consequence actions.
- **S5 — Append-only audit + Merkle ledger.** No user-space erase path; science ledger Merkle-chained; wraps flagged.
- **S6 — Fault isolation.** A ring-3 fault kills the agent, never the kernel; copyin/copyout on all user pointers.
- **S7 — Metric-gaming prevention.** The verifier agent is structurally independent; the objective function is kernel-locked.
- **S8 — Skill-change veto.** Any `skill.md` or code write requires `CAP_SOVEREIGN` + optional manual approval.

---

## Architecture prerequisite checklist (answer in the DDR before coding any Section E/F item)

1. New NSI/syscalls — is the append-only NSI range still open? (through 75; next append starts at **76**)
2. TCB / roster-slot / agent-slot fields — struct extension? ABI size assumptions?
3. PMM/VMM shared mappings — needs `PTE_SW_SHARED` (DDR-729 pattern) or a new mapping class?
4. Capability checks — which `CAP_*` gates it (existing or 1<<18..1<<23); is a new bit needed?
5. AETHER queue/audit extensions — new record type in `aether_audit.c`/`aether_queue.c` (Merkle chaining for S5, DAG `parent_action_id`)?
6. Scheduler/accounting hooks — `sched_exit`, `sched_create_state`, `agent_metrics_reap` (lineage departure summary, spawn-depth enforcement)?
7. Filesystem/root-mount constraints — depends on `root_mnt` (DDR-739), SFS dirs (DDR-738), or a new `fs_prefix`/namespace concept?
8. Network policy tables — touches `net_allow`, `CAP_NET`, `CAP_NET_BROWSE`, or the socket NSI (ADR-027)?
9. Compositor/UI exposure — new agent card/roster visual (Section G agents, Section F #73 sovereign UI)?
10. New smoke gate — what proves it **deterministically**? (avoid TCG-timing flakiness — DDR-735/771 lessons)
11. **Security invariant check (MANDATORY)** — which of S1–S8 govern this feature, and does the plan violate any of them even indirectly?

---

## Standing AETHER cognitive/ASI mandate

Long-term trajectory: subconscious background reasoning (F#71), verifier-based
self-improvement (F#72), the **metric lockbox** (F#68 / S3 — highest priority once
Section D is confirmed built, which it now is), and the full 12-agent Section G
roster. This is not a single task; it is an evolving capability goal tracked here.
Decompose into bounded DDRs only when a concrete slice is chosen.
