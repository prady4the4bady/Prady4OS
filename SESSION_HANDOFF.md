# PRADYOS — SESSION HANDOFF

> A fresh Claude Code session (possibly on a new account/machine) should read this
> file **in full** before touching anything. It captures the exact repo state and
> the non-negotiable contracts so the project continues with zero context loss.

---

## 0. RESUME INSTRUCTION (read this first, act in this order)

> **"NET-B (lwIP) AND Layer 6 (AETHER) are COMPLETE and CI-green. AETHER ships
> the agent layer (ADR-026 + DDR-AETHER): kernel action queue + append-only audit
> + per-process mem cap + syscall rate limit, 10 NSI calls (29–38), CAP_SOVEREIGN/
> CAP_AGENT, and the ring-3 daemon+agent — gates `smoke-aether`, `smoke-aether-
> queue`, `smoke-aether-sec` all pass (32 gates total). The **ring-3 socket NSI**
> (ADR-027) is also COMPLETE — 8 kernel proxy sockets + `SYS_SOCK_*` (39–42) +
> agent live mode (Ollama over HTTP), with the boot page tables relocated to
> 0x300000 for durable kernel-growth headroom; `make smoke-agent-live` is the
> developer-run live gate (CI stays test-mode). **Layer 7 has STARTED** with the
> Sovereign/Manual toggle binding (DDR-701): PRISM `mode` builtin + the daemon's
> `SYS_SET_MODE` self-check, gate `smoke-mode`. **Layer-7 slice 0 (VirtIO-GPU
> framebuffer, ADR-028) is COMPLETE** — `kernel/drivers/gpu/virtio_gpu.c` brings up
> scanout 0 + presents a BGRA framebuffer (gate `smoke-gpu` →
> `PRADYOS_GPU_FB_OK 1024x768`; 33 CI gates). The framebuffer prerequisite is
> satisfied. The **ring-3 framebuffer surface (DDR-702) is also COMPLETE** —
> `SYS_FB_INFO/MAP/FLUSH` (43–45) let a ring-3 program map the GPU front buffer,
> draw, and present (gate `smoke-fb` → `PRADYOS_FB_DRAW_OK`; 34 CI gates; stage-2
> load raised to 16 chunks/512 KiB). **PS/2 keyboard input (DDR-703) is also
> COMPLETE** — `SYS_INPUT_POLL` (46) + a real IRQ1 path, gate `smoke-input` injects
> keys via QEMU `sendkey` (35 CI gates). Ring 3 can now both **draw to the screen**
> and **read the keyboard**. **The in-house sovereign-desktop compositor (DDR-704)
> is also COMPLETE** — `user/compositor.c` (CAP_SOVEREIGN) renders the mode's
> desktop over the framebuffer with an embedded 8×8 font and flips Sovereign/Manual
> on the `s`/`m` keys (gate `smoke-compositor`, 36 CI gates). It is the sole FB
> consumer (fbtest was folded in). **The virtio-input pointer (DDR-705) is also
> COMPLETE** — `SYS_MOUSE_POLL` (47) + a virtio-tablet driver; the compositor draws
> a cursor on click (gate `smoke-mouse` via QMP `input-send-event`; 37 CI gates).
> **PRADYOS now renders a keyboard- and pointer-driven desktop that reflects the
> kernel mode.** **Per-client surfaces (DDR-706) are also COMPLETE** —
> `SYS_SURFACE_*` (48–52) + a 16-entry PMM surface table shared by physical mapping;
> `user/surfacetest.c` commits a window the compositor composites (gate
> `smoke-surface`; 38 CI gates). **Ring-3 apps can now render windows.** **Named-agent
> UI panels (DDR-707) are also COMPLETE** — the compositor renders the 8 named agents
> (KRYOS…SOLIN) as cards with active/inactive state tied to AETHER's 8-slot roster
> (new `SYS_AGENT_ROSTER` 53; daemon lights KRYOS); gate `smoke-agents`.
> **Surface z-order + focus + input routing (DDR-708) is also COMPLETE** — surfaces
> stack by `z`, one holds focus, and the compositor routes keys to the focused
> window (`SYS_SURFACE_RAISE`/`SENDKEY`/`GETKEY` 54–56); gate `smoke-focus` (40 CI
> gates). **Sun-driven OKLab ambiances + the animated toggle (DDR-709) are also
> COMPLETE** — the 4 time-of-day ambiances render with genuine OKLab interpolation
> (libm-free Newton-cbrt), selected by `SYS_CLOCK` (57, RTC seconds-since-midnight);
> gate `smoke-ambiance`. **Window decorations + drag-to-move (DDR-710) are also
> COMPLETE** — title bars + `SYS_SURFACE_MOVE` (58); drag a window by its title bar
> (gate `smoke-drag`, 42 CI gates). **PRADYOS now has direct-manipulation windows
> (stack, focus, key routing, drag-to-move) on a perceptually-correct time-of-day
> desktop with an agent roster.** Next: visual richness (particle fields / glass
> blur / gradients) or window close/resize + title strings. wlroots/Wayland remain
> out-of-tree ports (brief §12 7b+, the wall). Read this
> file in full, run `graph_session_primer()`, confirm gates green, and write the
> ADR/DDR before any code. Do NOT restart earlier slices."**

Concretely:
1. Read this whole file (esp. §0.1 — current state).
2. `graph_session_primer()` — or `node tools/graph_mcp/server.js primer`.
   (Graph node_modules already installed here; fresh clone: `cd tools/graph_mcp
   && npm ci && node server.js rebuild`.)
3. Run the full gate set (§6) **plus** `smoke-fpu smoke-init smoke-shell` and
   confirm green before editing.
4. **NET-B (lwIP) is COMPLETE and CI-green** (**ADR-025** + **DDR-NET-B**; lwIP
   2.2.1 pinned at `third_party/lwip` `77dcd25a`). liblwip.a (`-w -nostdlibinc`)
   is **linked into the kernel** alongside the first-party port
   `third_party/lwip-port/lwip_port.c` (`-Werror`): allocator/rand/diag/assert/
   `sys_now` shims, the virtio-net⇄netif bridge (`pradyos_linkoutput` TX,
   `pradyos_netif_rx`→`ethernet_input`), static IP 10.0.2.15/24 GW 10.0.2.2,
   `net_init` wired into kmain + `sys_check_timeouts`/`netif_poll_all` on the PIT
   tick. The NET-A RX drop was root-caused (buffer recovered from
   `vq->desc[head].addr`, delivered past the 12-byte hdr, descriptor re-armed; TX
   chains freed). Gates **`smoke-net-lo`** (`PRADYOS_NET_LO_OK`), **`smoke-net`**
   (host→guest TCP echo on :8007, `PRADYOS_NET_TCP_OK`), **`smoke-net-fuzz`**
   (malformed frames + SYN flood, no panic, `PRADYOS_NET_FUZZ_OK`) — all in CI.
   **Layer 6 (AETHER) is also COMPLETE and CI-green** (ADR-026 + DDR-AETHER):
   `kernel/aether/` (queue/audit/mem/rate, PMM-pool rings), NSI 29–38 in
   `kernel/syscall/sys_aether.c`, `CAP_SOVEREIGN`/`CAP_AGENT`, and the ring-3
   `user/aether_daemon.c` + `user/agent_base.c`. Gates `smoke-aether`,
   `smoke-aether-queue`, `smoke-aether-sec` in CI (32 gates total). Deferred:
   live Ollama (needs a ring-3 socket NSI), SFS config read, IPC console.
   **Begin Layer 7 (UI/UX) next, or a socket NSI for live inference.**

**5d/5e closed this session** (HEAD `9f310da`): per-thread FPU save (ADR-023 §D8),
pradyos-init PID 1 (ADR-023 §5d), and the **PRISM shell** (ADR-024) — interactive
ring-3 shell over the serial console, builtins help/echo/cat/run/ls/ps/exit, with
console RX (IRQ4 ring buffer) and **full-register fork** now in the kernel.

**Open follow-ups (deferred, see ADR-024 / build_status):**
- **FAT32 large-file read / `execve` of a large musl-C ELF corrupts** — PRISM is
  kernel-spawned from SFS instead; init-`execve` respawn waits on this fix.
- `ls`/`ps` are stubs (need `SYS_GETDENTS` / a process-table syscall); RX line
  discipline/echo; pipes/redirection/quoting/job-control/scripting.

### 0.-1 TASK TRACKER (authoritative; update EVERY loop — master-prompt §3)

- **CANONICAL FEATURE STATE:** `docs/AETHER_MASTER_FEATURES.md` (Sections A–H) is
  the single source of truth for feature status — created 2026-07-24. Never let a
  second feature list exist. Mirror it here + in `docs/build_status.md` in the
  same commit as any code touching agents/UI/sockets/storage/namespaces/telemetry/
  scheduling/capabilities.
- **LAST_COMPLETED_TASK (newest):** DDR-773 mkfs.sfs multi-leaf B+tree (Master doc
  Section B item 2). Slots now collected flat (bounded `MKFS_MAX_SLOTS = 512`,
  clean error = S2) and bulk-loaded: ≤14 slots → single leaf at block 1
  (byte-identical to before), else N leaves + one internal root with separators =
  each next leaf's first key (mirrors kernel `bt_search_root` descend).
  `sfs_readback` descends too. Gates: `smoke-mkfs-sfs` +20-file/41-slot case
  verifying first/middle/last; `smoke-sfs-persist` now feeds the KERNEL a
  multi-leaf image (`21 slots, root=23`) — both PASS locally. Host-tool only,
  zero kernel files. **106 gates. CI pending on dev/phase1.**
- **SECTION D VERIFIED (2026-07-24):** ADR-026 baseline #1–17 confirmed genuinely
  BUILT in `kernel/aether/` (QUEUE_LEN 256, AUDIT_LEN 4096, PAYLOAD_MAX 512,
  MEM_DEFAULT 128 MiB, RATE_MAX 60/window 100, CAP_SOVEREIGN 1<<16, CAP_AGENT
  1<<17, SYS_SUBMIT/APPROVE/REJECT/SPAWN_AGENT/KILL_AGENT, ACTION_WRITE_FILE/
  PRINT/SPAWN_PROCESS). No drift. Next free cap bit `1<<18` = CAP_MEMORY as
  planned. This is the precondition for starting any Section E/F work.
- **PRIOR:** DDR-772 NVMe PRP2 + PRP list. `nvme_io` now
  issues multi-page single commands instead of one-per-≤page-chunk: `nvme_submit`
  gained a `prp2` arg; a scratch PRP-list page (`n->prp_list`) holds ≤512 page
  addrs. Per command PRP1 + (0 | second-page | PRP-list), capped ~2 MiB, loop for
  larger. `smoke-nvme` extended: 16 KiB round-trip as ONE command → gate PASS
  (5 patterns incl. `PRADYOS_NVME_PRP_OK`). **106 gates. CI pending.** NVMe IRQ
  still deferred (needs an MSI-X vector; 50–63 window full).
- **PRIOR:** ADR-032 FS write budget lifetime-cap →
  token-bucket rate limit. `fs_write_budget` is now a bucket refilling
  256 KiB/tick (25 MiB/s) up to a 1 MiB burst cap; added `tcb.fs_budget_tick`;
  `vfs_write` lazily refills from elapsed ticks (only tops up below-cap; never
  reduces → preserves the `~0ull` kernel bypass). Removes the 1 MiB LIFETIME
  ceiling; bounds RATE (anti-flood); disk-space is a separate control. New gate
  `smoke-fs-budget` — deterministic (rewind `fs_budget_tick` to simulate elapsed
  ticks): drained bucket refills+writes (old cap couldn't) AND rejects with no
  elapsed time. churn (smoke-sfs-btree) + budget bypass regressions green. **105
  gates. CI pending on dev/phase1.** LESSONS: (1) `make image` doesn't always
  rebuild `main.o` on source change — `rm build/main.o` before local test builds
  (wasted cycles on stale kernels); (2) a single 64 KiB FAT `vfs_write` hung in
  this context — used 4 KiB; (3) grep gate sentinels when changing kernel prints.
- **PRIOR:** DDR-771 `VBLK_MAX` 4→8 (MSI-X vector remap) —
  virtio-blk block MSI-X window moved 50–53 → 56–63 (clear of net@54/input@55).
  Required extending shared MSI-X infra: `isr.asm` stubs + `isr_stub_table`→64,
  `idt.c` gate loop→64 + `MSIX_VEC_COUNT` 6→14, plus `virtio_blk.c` VBLK_MAX 8 +
  base 56 + handlers 4–7. Root-caused a #GP (err 0x1C2 = IDT vec 56 ungated) a
  driver-only change first hit. `smoke-aether-sfsroot` now boots 5 virtio-blk
  disks (dropped `QEMU_NO_EXT4`, added ext4-image): asserts `blk4 ready` + daemon
  roots at provisioned image at blk4 alongside ext4. Local PASS. **104 gates. CI
  pending on dev/phase1.**
- **PRIOR:** DDR-770 persistent root from a host mkfs.sfs
  image — the AETHER `/etc/aether/config` now ships in `build/sfsroot.img`
  (mkfs.sfs, `QEMU_SFSROOT` knob) instead of being kernel-provisioned. The
  DDR-768 peek records the provisioned disk's mount (`prov_mnt`) when its
  `/etc/aether/config` starts with `mode=`; the persistent-root block roots the
  AETHER daemon there and skips the kernel `vfs_create/write` of config. Default
  boots unchanged (blk2 fallback intact). DDR-769's nested marker moved to
  `/etc/test/config` to disambiguate. New gate `smoke-aether-sfsroot`: `AETHER
  daemon rooted at provisioned mkfs image` + `PRADYOS_AETHER_CFG_OK
  mode=sovereign`. Local PASS (+ smoke-sfs-persist regression green). **103
  gates. CI pending on dev/phase1.**
- **PRIOR:** DDR-769 nested-directory provisioning in host
  `mkfs.sfs` — `add_file(path,…)` walks `/`-split components (intermediates =
  dir inodes `SFS_INO_DIR`, no extents, dedup shared prefixes; last = file),
  slots keyed `(parent<<32)|FNV1a32(name)` in the single root leaf; `sfs_readback`
  walks the same. `smoke-sfs-persist` extended: mkfs provisions
  `/etc/aether/config`, kernel reads it → `PRADYOS_SFS_NESTED_OK`. Verified
  locally (host round-trip + kernel boot). **102 gates. CI pending on dev/phase1.**
  NEXT after this lands: DDR-770 (migrate persistent-root `/etc/aether/config`
  off kernel provisioning onto a shipped mkfs image — touches boot/root flow).
- **PRIOR:** DDR-768 cross-reboot SFS persistence proof —
  the kernel boots on a host `mkfs.sfs` image and reads `/PERSIST.TXT` back.
  Guarded self-test in `fs_test_thread` (`main.c`): peek highest blk index's
  block 0 for `SFS_MAGIC` (`blk_read`), mount, `vfs_open`/`vfs_read`
  `/PERSIST.TXT` → `PRADYOS_SFS_PERSIST_OK`. `boot_test.sh` gains `QEMU_SFS2`
  (attach mkfs last) + `QEMU_NO_EXT4`. Root-caused a gotcha: virtio-blk caps at
  `VBLK_MAX=4` (MSI-X vec 50–53 packed vs net@54/input@55 → can't naively raise);
  with ext4 present the mkfs disk was the dropped 5th → suppress ext4 for this
  gate instead. `VFS_MAX_MOUNTS` 4→6. New gate `smoke-sfs-persist`, local PASS.
  **102 gates. CI pending on dev/phase1.**
- **PRIOR:** DDR-767 host `mkfs.sfs` — `tools/mkfs_sfs/{mkfs_sfs,sfs_readback}.c`
  #include kernel `sfs.h` (byte-exact). Host gate `smoke-mkfs-sfs`. Landed with
  the 16 `-smp 4` timeout bumps (90→180/60→120) that cleared the surfdestroy
  flake. **CI-green on `main` at `4ebcdc4`.** 101 gates.
- **PRIOR:** DDR-765+766 NVMe (controller bring-up + block I/O + `blk_register`)
  — `nvme0` registered, `PRADYOS_NVME_RW_OK`. Both **CI-green on `main` at
  `4ebcdc4`** (the 767 run validated 765+766+767 cumulatively after the -smp4
  timeout bump cleared the surfdestroy flake).
- **DDR-773 IS CI-GREEN ON `main` @ `3485085`** (run 30132610321, 106 gates).
- **DDR-774 (scoping only, no code):** blast-radius review of B#1 NVMe IRQ found
  it is NOT bounded — split into 774a/b/c. Vector availability was never the
  blocker. (a) the only MSI-X programmer is virtio-coupled
  (`virtio_pci_msix_setup` takes `struct virtio_pci_dev*`, uses virtio `map_bar` +
  `common_cfg.queue_msix_vector`) so it must be refactored out of a path serving
  blk/net/gpu/input — the same shared surface that produced DDR-771's #GP and
  stale-sentinel CI break; (b) `nvme.c` maps a fixed 2-page BAR0 window but the
  MSI-X table offset/BIR is runtime-determined and may be outside it or in another
  BAR; (c) completion moves thread→IRQ context for every I/O incl. the DDR-772 PRP
  loop. Per policy, invasive work was NOT started.
- **DRIFT CORRECTED:** master-doc Section B#8 (`ls`/`ps`) was STALE — both are
  already implemented in `user/prism.c` (`ls`→SYS_GETDENTS/DDR-742,
  `ps`→SYS_GETPROCS/DDR-743). Moved to Section A.
- **CI INFRA FLAKE (2026-07-25):** run 30139119085 (docs-only commit a02e790)
  FAILED at the `Install toolchain` step — NOT a regression: rustup reported
  `checksum failed for channel-rust-nightly.toml` → `toolchain 'nightly-...' is
  not installable`. Upstream static.rust-lang.org publish/CDN issue. `main` was
  therefore NOT promoted to a02e790. The DDR-774a push re-attempts the install; if
  this recurs, consider pinning rust to a dated nightly or stable in the workflow
  (the rust dep exists only for the `toolchain-check` no_std interop gate).
- **LAST_COMPLETED_TASK (newest):** DDR-774c **phase c-1** — NVMe MSI-X delivery
  ROOT-CAUSED AND FIXED (Master doc Section B#1 sub-slice c). The 774b symptom
  `[nvme] irqs=0` was **my bug in 774b**: NVMe `Create I/O CQ` CDW11[31:16] is an
  **index into the MSI-X table**, NOT the x86 interrupt vector. It was passed 50,
  aiming the controller at an unprogrammed/masked entry while only **entry 0** was
  programmed (the x86 vector rides in that entry's message-data field). Fix =
  pass the table index: `NVME_MSIX_ENTRY 0`. Result: **`[nvme] irqs=6`**.
  IMPORTANT: the leading suspect (MSI-X **Function Mask**, MC bit 14, never
  explicitly cleared) was **NOT** the cause — so `pcie_msix_program()` (the SHARED
  774a helper) was left untouched and the four virtio consumers cannot regress.
  Neither was the table address math / BIR decode. `smoke-nvme` PASS with 7
  patterns, adding `PRADYOS_NVME_IRQ_OK` (forbidding `..._IRQ_FAIL`), made
  deterministic by a BOUNDED settle spin (S2) instead of a timing-dependent count;
  the exact count is printed but NOT asserted (brittle if the self-test changes).
  **106 gates. CI pending.**
- **PRIOR:** DDR-774b NVMe MSI-X table mapping + IEN
  (Master doc Section B#1 sub-slice b) — plumbing only, completion STILL POLLED.
  `nvme_msix_setup()` uses `pcie_msix_find()` for the runtime BIR/offset, maps 2
  uncached pages at `NVME_MSIX_VBASE` (separate from the BAR0 register window),
  programs entry 0 for **vector 50**, registers an inert counting ISR, disables
  INTx, and creates the I/O CQ with IEN + vector in cdw11. `smoke-nvme` PASS
  (6 patterns incl. the deterministic `[nvme] msix vec=50`); RW_OK/PRP_OK green so
  the polled path is unaffected. **106 gates. CI pending.**
  ⚠ **OPEN ISSUE — MSI-X delivery does NOT work: boot prints `[nvme] irqs=0`.**
  Ruled out: interrupts masked (`sti` at main.c:1478/:1737 precedes `nvme_init` at
  :1801, so IF is set). Suspects for 774c, in order: (1) MSI-X **Function Mask**
  (message-control bit 14) is never explicitly cleared by `pcie_msix_program` —
  it is 0 after reset so virtio works, but NVMe's state is unverified;
  (2) the table address math in `nvme_msix_setup` (no #PF occurred, so the write
  hit mapped memory — but not provably the controller's table); (3) per-entry
  vector control read-back. The count is printed but deliberately NOT gated
  (timing-dependent while polling). **774c MUST root-cause delivery before
  touching the completion path.**
- **PRIOR:** DDR-774a generic PCI MSI-X programmer (Master
  doc Section B#1, sub-slice a) — pure refactor. `pcie_msix_find()`,
  `pcie_msix_program()`, `pcie_intx_disable()` added to `kernel/drivers/pcie/pcie.c`
  (+ decls in `pcie.h`); `virtio_pci_msix_setup()` reimplemented on them with
  identical signature, register writes and ORDER (INTx-disable stays at the call
  site so it still runs after the per-queue routing). All three virtio callers
  unchanged → the DDR-774 stop condition ("if it forces driver changes, stop") did
  NOT trigger. Unblocks 774b. Gates: smoke-fs/net/input/gpu.
- **DDR-774b IS CI-GREEN ON `main` @ `2034a53`** (run 30146543550); DDR-774a landed
  earlier at `347c422` (run 30141466540, which also carried the a02e790 docs
  commit). The `Install toolchain` rustup failure was confirmed transient — no CI
  pinning change was needed.
- ✅ **`main` IS AT `2083c09`** (run 30158060606 GREEN) — that promotion carried
  DDR-774c c-1 + both docs commits out of limbo. Section B#1 (NVMe MSI-X) is
  functionally complete for correctness and fully on `main`.
- **LAST_COMPLETED_TASK (newest):** DDR-776 virtio-blk stuck-request watchdog
  (Master doc Section B#3) — **diagnosis, NOT a fix; do not describe it as one.**
  `struct vreq` gains `t0`/`lba`/`warned`; `virtio_blk_watchdog()` is called from
  the timer path in `idt.c` every ~1 s (same idiom as `net_poll_tick`) and prints
  `[vblk] stuck dev=D slot=N lba=L age=T` once per request stuck >5 s. Works while
  a submitter is blocked forever because only that thread is stuck. No blocking
  behaviour changed; NO lock taken (read-only from the ISR — deliberately avoids
  adding a deadlock surface to the subsystem under investigation, S6); bounded
  64 checks/tick + one print per request (S2). **Design decision recorded:**
  yield-loop REJECTED (would spin on every block I/O, regressing the hot path all
  FS gates ride); scheduler timed-block DEFERRED (correct eventual primitive, but
  changing the scheduler core mid-investigation would confound attribution).
- **(historical) CI RED episode — main was NOT promoted.** Run 30151522978 (commit `715040e`, DDR-774c
  c-1) FAILED at step 101 `smoke-surfdestroy` (q35 `-smp 4`) — the Section B#3
  race. NOT a regression from 774c: that gate boots with **no NVMe device** (so
  `nvme_init` never runs), `smoke-nvme` passed earlier in the same run, and the
  same gate failed back in run 29726803735 (DDR-766, before any 774 work).
  **New signature:** timed out at the FULL 180 s having missed the FIRST sentinel,
  serial shows the boot **HUNG after `SYSFSTAT OK`** (ring-3 syscall self-tests,
  before any surface test) → a **hang, not slowness**; the DDR-771 timeout bump is
  not a fix. `main` stays at `2034a53`; `715040e` is unpromoted on dev/phase1.
- **DDR-774c c-2 STOPPED + re-scoped (no code)** — stop condition invoked; see
  DDR-774c. `nvme_submit()` is already a bounded poll, so the specified
  no-scheduler-hook design is polling-with-a-hint; a genuine IRQ wait needs the CPU
  to sleep (`sti;hlt` rejected — mutates caller `IF`; guarded `hlt` idle; or a
  scheduler block/wake wait-queue = out of scope). Deferred to a future DDR that
  must MEASURE polling cost first. Section B#1 is functionally complete for
  correctness (programmed + delivered + gated).
- **CURRENT_ACTIVE_TASK:** none — `715040e` awaits a green run.
- **DDR-775 (findings, no code): B#3 NARROWED to the virtio-blk completion wait.**
  Second CI hit — run 30155872016 failed `smoke-blk-integrity` (`-smp 4`,
  concurrent read data-verify) at the full 180 s. Local repro attempted: 3/3
  `smoke-surfdestroy` PASS → CI-only timing window. Two different `-smp 4` gates,
  both block-I/O; surfdestroy's stall point (`SYSFSTAT OK`, next `SYSREAD OK`) is
  inside `sys_read`→`vfs_read`→SFS→virtio-blk. **CONFIRMED DEFECT + S2 VIOLATION:**
  `submit()`'s `while (!v->req[s].done) sched_block_on(&v->compl_lock);` is
  UNBOUNDED → a missed completion hangs forever instead of erroring. The
  lost-wakeup race is NOT the defect (locks-4 pattern correctly closes it).
  **Latent, not this trigger:** `slot_waiter` is a single `struct tcb *`, so
  >VBLK_NREQ(8) concurrent submitters lose a wakeup. NO fix shipped — it does not
  reproduce locally, so a concurrency change to the shared block path would be
  pushed unvalidated and judged only by a 2.5 h CI run.
- ⚠ **CI RED on `c61a149` (run 30163444702) — DDR-776 unpromoted; `main` stays at
  `2083c09`.** Failing gate: step 51 `smoke-smpuser` "user-on-AP" (`-smp 4`,
  ring-3 thread on a non-BSP CPU), 180 s timeout, missed `[smp] user on AP OK`.
- 🔬 **DDR-776's FIRST RESULT IS A NEGATIVE ONE THAT REFUTES THE DDR-775
  NARROWING.** The watchdog printed **nothing** in that failure — no virtio-blk
  request stuck >5 s — while the timer was demonstrably still firing (boot
  progressed through the fuzz test, a ring-3 thread exited). So this failure is
  NOT a stuck block request. The three B#3 failures now share only `-smp 4` and
  miss DIFFERENT sentinels: surfdestroy (hung after `SYSFSTAT OK`),
  blk-integrity (`[smp] blk integrity OK`), smpuser (`[smp] user on AP OK`).
  **Revised: the ORIGINAL percpu-scheduler/AP-race framing is better supported
  than the virtio-blk narrowing** (DDR-775's narrowing section is marked
  superseded). Hazard 1 (unbounded completion wait) and Hazard 2 (single-element
  `slot_waiter`) are still real S2 defects worth fixing on merit — but are NOT
  proven to be this trigger. Note this vindicates diagnosis-first: a blind bound
  on the blk wait would have fixed nothing and masked the real cause.
- ⚠⚠ **B#3 SECOND CORRECTION — LEADING HYPOTHESIS IS A STALLED TIMER.** In run
  30163444702 the serial printed **neither** `[smp] user on AP OK` **nor**
  `[smp] user on AP FAIL` (the 2 log hits are the sentinel echo + the "not found"
  message, NOT serial), yet the preceding `ap preempt OK` / `resched OK` DID
  print. `smpuser_proof()` (main.c:659) is `while (!g_user_on_ap && g_ticks < dl)`
  — a deadline poll that MUST print one branch **unless `g_ticks` stops
  advancing**. This RETRACTS my earlier inference: the DDR-776 watchdog runs on
  the SAME timer path, so its silence cannot distinguish "no stuck blk request"
  from "the watchdog never ran"; my "timer was demonstrably firing" claim rested
  on boot progress that happened EARLIER than the stall point.
  **Hypothesis: under `-smp 4` the timer tick intermittently stops advancing
  `g_ticks`.** Explains all four failures at once (each missing whichever
  sentinel the boot had reached), the watchdog silence, blk waits never waking,
  and consistent local passes. **Systemic S2 exposure: EVERY `g_ticks`-bounded
  wait in the tree is only as bounded as the timer.**
- ⚠⚠⚠ **THIRD CORRECTION — "timeout ⇒ hang" WAS WRONG.** `boot_test.sh` ALWAYS
  runs QEMU for the full `TIMEOUT_S` window and THEN greps; `terminating on
  signal 15 … (timeout)` appears in PASSING runs too (verified in a local
  smoke-input PASS). So none of the B#3 failures are proven to be hangs — the
  only hard evidence is **sentinel absent**. Do not describe them as hangs.
- **NEWLY ESTABLISHED:** every SMP proof shares `if (!g_smp_have_aps) return;`
  (silent early return). In the failing run `ap preempt OK` + `resched OK` DID
  print ⇒ `g_smp_have_aps == 1` ⇒ `smpuser_proof()` did NOT early-return; it
  entered the poll and never reached its `kputs`. Three explanations survive:
  **(A)** timer stalls so `g_ticks < dl` never expires; **(B)** scheduler
  starvation — never resumes from `yield()` while the system stays alive;
  **(C)** guard/ordering effect.
- **LAST_COMPLETED_TASK (newest):** DDR-777 three-way discriminator probe
  (Section B#3) — **passive instrumentation, NOT a fix.** `[hb] t=<g_ticks>` every
  ~500 ticks from the existing timer call site in `idt.c`, plus
  `[smp] user-on-AP probe t=…` at `smpuser_proof` entry and a tick appended to the
  OK/FAIL line. Sentinel safety verified: `boot_test.sh` uses `grep -qF`
  (substring), so EXTRA `[smp] user on AP OK` and FORBIDDEN `user on AP FAIL`
  both still match with the ` t=<n>` suffix. No behaviour change, no locks, no
  scheduler hook (S2/S6).
  **HOW TO READ THE NEXT FAILING RUN (this is the whole point):**
  no `probe t=` line ⇒ **(C)** APs never came up (silent guard);
  `probe t=` present + `[hb]` STOPS ⇒ **(A)** timer stalled → B#3 moves to the
  LAPIC/timer path, and *every* `g_ticks`-bounded wait in the tree is exposed
  (systemic S2);
  `probe t=` present + `[hb]` CONTINUES + no OK/FAIL ⇒ **(B)** scheduler
  starvation → a runqueue/AP-claim bug.
- ✅ **`main` IS AT `968169c`** — all of DDR-774c c-1, DDR-776 (blk watchdog),
  DDR-777 (discriminator probe) and every B#3 correction doc are promoted.
  `dev/phase1` and `main` are level.
- **B#3 = INSTRUMENTED, AWAITING A NATURAL FAILURE.** Four consecutive green runs
  (30158060606, 30165570464, 30167716462, 30170362044) after the cluster of four
  failures; never reproduces locally (3/3). The probe is on `main` and will
  explain the next failure by itself. **Do NOT force a fix** — that would be a
  fourth blind attempt (already refuted: DDR-771 timeout bump, the virtio-blk
  theory, the "timeout ⇒ hang" reading). On ANY future red run, grep for
  `user-on-AP probe t=`, `[hb] t=`, and `vblk] stuck` and read the 3-way verdict
  in DDR-777.
- 🔎 **SECTION B AUDIT (2026-07-26) — the roadmap overstated remaining work.**
  Verified against the tree (not assumed): **B#5 COW fork is SHIPPED**
  (`kernel/mm/vmm_cow.c`, `vmm_cow_fault` at `idt.c:225`, gate `smoke-cowfork`
  Makefile:760 / CI:228), **B#7 kernel self W^X is SHIPPED**
  (`vmm_protect_kernel()`, DDR-757 gate Makefile:660 + CI PTE audit), and B#8
  (`ls`/`ps`) was corrected earlier. All three moved to Section A. **The other
  Section B entries have NOT all been re-verified — check the tree before
  planning any of them.**
- **LAST_COMPLETED_TASK (newest):** DDR-778 PRISM output redirection
  (Master doc Section B#12, first bounded slice) — **ring-3 only, zero kernel
  change.** Scoping confirmed `SYS_DUP2 = 18` and `SYS_PIPE` already ship
  (PROC-A, gates `smoke-syspipe` + CI "pipe/dup2"); `prism.c` had merely never
  `#define`d `SYS_DUP2`. `cmd … > file` now works: scan `argv[1..argc)` for a bare
  `>` (from index 1 so a leading `> file` can't leave `argv[0]` undefined),
  truncate `argc`, then `dup2(1,REDIR_SAVE_FD)` / `open(O_CREAT|O_WRONLY)` /
  `dup2(fd,1)` / `close`, restoring at the loop's existing flush point.
  TWO HAZARDS HANDLED — (1) musl FULLY BUFFERS a non-tty stdout, so the flush must
  happen BEFORE fd 1 is restored or the output lands on the console and the
  redirect silently half-works; (2) a skipped restore would send ALL later shell
  output into the file, so control flow was audited (only `continue` precedes the
  swap; `ls`'s `break` is an inner loop; `exit` returns from main). Gate:
  `smoke-shell` + a DISCRIMINATING PAIR — the marker alone would pass even with
  redirection broken (plain `echo` prints it), so `REDIR.TXT` must ALSO appear in
  `ls /`. **106 gates. CI pending.**
- 🚨 **CI IS BLOCKED UPSTREAM — `git.musl-libc.org` IS DOWN (DDR-779).** Run
  30178367399 (`21e4a51`, DDR-778) failed after 4m39s at **step 2
  `actions/checkout@v5`**, before any project code was fetched: the
  `third_party/musl` submodule clone timed out twice. Verified independently —
  `git ls-remote https://git.musl-libc.org/git/musl` is unreachable from here too.
  **NOT a regression, and pushing more commits cannot help** — every run will die
  at checkout until musl returns. This is the SECOND external-dependency outage
  this session (the first was `static.rust-lang.org`'s nightly checksum, run
  30139119085, which self-resolved). `.gitmodules` pins musl to a single upstream
  host while lwip already uses GitHub — that asymmetry is the single point of
  failure. **Proposed fix (NOT applied, needs maintainer sign-off — it is a
  supply-chain change): point musl at a GitHub mirror keeping the same pinned SHA
  `0784374d…`.** Content integrity is SHA-guaranteed, but the mirror must first be
  confirmed to contain that exact commit via a real fetch (`git ls-remote` only
  lists ref tips and returned inconclusive, not negative). See DDR-779 for the
  verification recipe and alternatives (vendor / actions/cache / retried step).
  **Operational rule while down: check step 2 before diagnosing any CI failure,
  and do not push just to retrigger.** Local work is unaffected (musl is already
  cloned; `make image` and all local gates run normally).
- ⚠ **LOCAL-ONLY GATE LIMITATION (not a regression): `make smoke-shell` fails
  here with `no PRISM_READY`.** The gate writes its serial log to `build/` on
  **DrvFs (`/mnt/c`)**, which is slow enough in this WSL setup that the boot
  misses the gate's 60 s window. Evidence it is environmental: a direct boot
  prints `PRISM_READY` fine, and `smoke-shell` **passes in CI** (step 26, green in
  run 30170362044). Do NOT "fix" this by raising the gate's timeout. To validate
  shell changes locally, use the same FIFO flow but put the serial log on `/tmp`
  (ext4) — see the DDR-778 proof script pattern.
- 🧪 **LOCAL-VALIDATION GOTCHA (cost a wasted check):** WSL's `/tmp` does **not**
  persist between separate `wsl -d Ubuntu-24.04 bash -lc ...` invocations, so a
  serial log written by one call is GONE in the next — post-hoc inspection is
  impossible. Put the boot AND every assertion in ONE scratchpad script. Also note
  an unexpanded `$VAR` inside nested `wsl -c` quoting makes `grep` error, and an
  `||` fallback then prints a FALSE "PASS" — use literal paths and `if/else`.
- ✅ **CI UNBLOCKED (2026-07-26): `git.musl-libc.org` is REACHABLE again.** The
  DDR-779 outage is over; runs get past `actions/checkout@v5` once more. The
  backlog (`21e4a51` DDR-778 `>`, `d8d78e4` DDR-779 finding, `6561a74` DDR-780
  `|`, plus DDR-781) can finally be CI-validated — promote `main` to the first
  green sha covering them. DDR-779's mirror proposal still needs sign-off; do NOT
  apply it autonomously.
- ✅ **PROMOTED (2026-07-26): `main` fast-forwarded to `ac459d7`** on CI run
  **30193738689** (`success`, 112/112). Promotes DDR-782 (kernel `O_TRUNC` +
  atomic `O_APPEND`), ADR-033/DDR-779 (musl mirror) and DDR-783. That run also
  confirms both fixes individually: **step 2 checkout passed** after three musl
  outages, and **step 10 smoke-fs passed** after the measured timeout bump.
## AETHER host-side Python layer (ASI-bridge v4.0) — build tracker

Root: `aether/`. Host-side Python 3.13 agent stack that runs ON the OS as a
service; NOT code the bare-metal target executes. Its invariants are **S1–S14**
(`aether/kernel/invariants/core_invariants.py`) and are **independent of the C
kernel's S1–S8** — the numbering collides, the meanings do not, never merge them.

**Gate command:** `python -m pytest -W error -x -q aether/tests/`
**CI:** the `aether-layer` job runs it on every push (added 2026-07-28 — before
that, "CI green" on an `aether/` commit proved only that the C kernel still built).

| Section | Items | Status |
|---|---|---|
| **B** (foundation) | B-01…B-17 | ✅ **COMPLETE** — 187 passed, 1 skipped, 0 warnings |
| **C** (gap features) | C-01…C-10 | ✅ **COMPLETE** — 335 passed, 1 skipped, 0 warnings |
| **D** (ASI bridge) | D-01…D-15 | 🔨 D-01…D-05 ✅ done; D-06…D-15 pending |
| **I** (integration) | I-01…I-10 | ⬜ not started |
| **J** (Phase 4 audit) | J-01…J-06 | ⬜ not started |

B-series detail: B-01 lockbox · B-02 audit log · B-03 merkle · B-04 firewall ·
B-05 quarantine · B-06 red team · B-07 agent bus · B-08 notification bus ·
B-09 model manager · B-10 computer use · B-11 OOBE · B-12 mesh · B-13 soul ·
B-14 vision · B-15 self-model · B-16 failure analysis · B-17 blind spots.

**Naming deviation:** the spec says `ai-core/`; hyphens are illegal in Python
package names, so it is `aether/ai_core/`.

**One test skips on Windows:** B-05's symlink-escape case needs a privilege this
dev box lacks. It is B-05's discriminator, so it was verified separately under
WSL. It runs normally in CI on Linux.

---

- ✅ **BUG-1 (-smp 4): FIRST VALID DDR-777 READ — verdict (B), 2026-07-28.**
  Run **30323686134** (`smoke-swapgs`, -smp 4, head `ff6d1d4` which is PYTHON-ONLY,
  so the commit under test cannot be the cause) missed `[percpu] gs OK (syscall ctx)`.
  Discriminator read from the SERIAL DUMP (not the Makefile echo):
  probe `t=152` **present** and `user on AP OK` **present** (rules out (C));
  **23 heartbeats t=500→11500, gaps uniformly 500** (rules out (A) timer stall);
  no `[vblk] stuck`; no KHEAP PANIC (so **not** BUG-0).
  **Verdict (B): the BSP boot thread stopped progressing; the timer did not.**
  ALL userspace output lands before the first heartbeat (<5 s), then 115 s of
  nothing but heartbeats. Neither `gs OK` nor `gs FAIL` printed, proving the
  one-shot probe block was never entered rather than having run and failed.
  **DDR-776's negative is now ADMISSIBLE:** the heartbeat rides the same timer
  path and fired 23 times, so the watchdog provably ran ~115 times — its silence
  is real evidence that **no virtio-blk request was stuck**, removing virtio-blk
  from the suspect list on evidence rather than assumption.
  Last self-test region reached is the SFS churn/GC block in kmain, and the churn
  reported **FAIL** in this run. NO FIX PROPOSED — three theories were already
  refuted here by acting before the mechanism was named. Next step: an OPT-IN
  BSP-liveness marker in that region (same pattern as PIPE_TRACE so it cannot
  evict gate markers).
- ⚠️ **CORRECTION to the handoff briefing:** the DDR-790 pipe create/destroy
  traces are **NOT active in CI** — they were made opt-in (`PIPE_TRACE=0`) in
  `c068d8e` because unconditional traces evicted `smoke-dmesg`'s log-ring marker.
  "Read the traces from the next CI run" cannot work until a run is built with
  `PIPE_TRACE=1`. Also: the current CI red is `smoke-swapgs`, not `smoke-blkmq`;
  `8bfbad0` was kernel-GREEN.
- 🚨 **OPEN REGRESSION — DDR-790: kernel heap DOUBLE-FREE PANIC in CI, on `main`.**
  Run **30215987521** (`ba5770e`) died at step 54 `smoke-blkmq` (-smp 4) with
  `[kheap] double-free ptr=0x7E29F80 objsize=0x20` / `*** KHEAP PANIC ***`.
  **Prime suspect DDR-787** (pipe refcount split) — `struct pipe` is 24 B, the
  32-byte bucket — and DDR-787 **is already promoted to main**.
  TWO MISREADINGS CORRECTED ALONG THE WAY, both worth remembering:
  (a) `multi-inflight FAIL` in the log is the Makefile echoing the gate's
  `FORBIDDEN_SENTINEL=` line, NOT a kernel print; (b) zero `[hb]` heartbeats
  looked like the DDR-777 timer-stall verdict, but the run PANICKED — so the
  silence is a consequence, not evidence. **B#3 was NOT validly read here and
  remains open.**
  Diagnostic added (temporary): `[pipe] create/destroy p=… r=… w=…`.
  **It has NOT found the bug.** Its first reading — three pointers each freed
  twice — looked conclusive but was **kheap ADDRESS REUSE**; adding the paired
  create trace showed creates=4/destroys=4, perfectly balanced. *Pointer identity
  is not evidence of a double free; create/destroy pairing is.*
  Not reproduced locally: 3/3 smoke-blkmq clean, trace clean.
  One hardening applied and labelled as such, NOT as the fix: `pipe_close` now
  frees only if the call actually DROPPED a reference (the first cut freed
  whenever both counts merely READ 0, so a close decrementing nothing could free
  twice; the old single refcount masked that shape by going negative).
  **NEXT: let CI run with the traces; a `destroy` with no matching live `create`
  names the pipe, and its absence exonerates it and points at `struct vfs_file`.**
- **(previous) LAST_COMPLETED_TASK:** DDR-789 — PRISM **exit status `$?`**
  (Section B#12). **The tree check REORDERED the queue:** SIGPIPE was next, but
  (1) signal defaults are a WHITELIST not a table (`signal_deliver` terminates on
  SIGKILL, and on SIGTERM with no handler, and IGNORES everything else — so
  defining SIGPIPE 13 and raising it would silently do NOTHING), and (2) SIGPIPE
  cannot be gated discriminatingly today: PRISM has no `head`-like builtin and a
  stage's outcome is unobservable, so "writer killed" vs "writer ran on into a
  dead pipe" look identical. `$?` makes outcomes observable (a signal-killed
  thread exits -1), so it lands first AND unblocks SIGPIPE.
  Ring-3 only — `sys_wait4` already returns the raw exit code and PRISM already
  collected it and threw it away. Pipeline reports its LAST stage.
  **Design changed under test:** the draft expanded a token equal to exactly `$?`,
  which testing showed useless (the idiom `echo status=$?` is ONE token), so it
  widened to a token ENDING in `$?` — the gate was not contorted to fit the
  design. Two incidental fixes: `snprintf` is NOT in the musl subset (same gap as
  DDR-784's stderr) so a bounded `fmt_long` replaces it; and DDR-786's stage loop
  read wait4 into a `long` while the kernel copies out `sizeof(int)` — upper bytes
  were uninitialised, now `int`.
  Local: st-ok=0, st-fail=127, no literal `$?`, DDR-786/787 regressions intact
  (200/200 big pipe), zero panics/warnings.
  **NEXT: SIGPIPE is now gate-able** — a killed stage exits -1, distinguishable
  via `$?`. It still needs SIGPIPE added to the default-terminate list.
- **(previous) LAST_COMPLETED_TASK:** DDR-788 — retire the DDR-783 flake class now
  that DDR-785's early exit makes margin FREE for eligible gates. Measured scope
  first and it was smaller than assumed: of 92 boot_test.sh invocations, 38
  declare FORBIDDEN_SENTINEL (untouched — for them the timeout IS the runtime, so
  raising them would add ~57 min to every green run), 43 already had an explicit
  TIMEOUT_S, leaving **11** on the default.
  **A first-draft claim was corrected by measurement:** I asserted two gates were
  at risk; only **`smoke-fs-sfs-rw`** actually is (30 s against a 30 s window — it
  asserts the journal/version-isolation/compress chain DDR-783 timed at
  24.09–24.26 s). `smoke-fs-rw` measured **5 s** and was never at risk.
  Seven gates raised to TIMEOUT_S=120; three left alone on purpose (`smoke` ×2
  asserts only NEXUS KERNEL OK at t=0.31 s; `smoke-mkfs-sfs` is host-side).
  "Free on success" was CHECKED, not assumed: 4/3/4/5/30/34 s, all PASS under the
  120 s ceiling and all against the DDR-787 kernel. Cost on failure is real and
  stated: a failing eligible gate now burns 120 s instead of 30 s.
- **(previous) LAST_COMPLETED_TASK:** DDR-787 — **blocking pipe semantics**
  (kernel). Fixes a LATENT CORRECTNESS BUG in shipped DDR-780/786 behaviour: the
  `FD_PIPE` read path returned 0 whenever the ring was momentarily empty and every
  reader treats 0 as EOF, so `a | b` was TIMING-DEPENDENT — `b` printed nothing if
  scheduled first. Those pipeline gates were passing by scheduling luck.
  Neither that nor the >4 KiB silent truncation was fixable on the old
  `struct pipe`: a SINGLE `refcount` cannot tell readers from writers, and a
  reader may block only while a writer remains (and vice versa), so blocking
  without those counts would be UNBOUNDED — a direct S2 violation.
  Now: `refcount` → `readers`+`writers`, the end passed explicitly at all six
  incref/close sites (`pipe.c`, `fd_free`, `fd_clone`, `sys_dup2`) plus
  `pipe_destroy` for the never-installed error path; frees only when both hit 0.
  Reader waits while empty && writers>0 (and only when it has delivered nothing
  yet); writer waits while full && readers>0, else `-EPIPE` (**`EPIPE` did not
  exist in errno.h** — added as 32). Waiting via the `yield()` poll already used
  for FD_CONSOLE reads: no wait queue, no new scheduler hook, termination always
  the refcount condition and never a timeout.
  **Second bug found while implementing:** the writer's `if (w < chunk) break;`
  dropped the remainder of a partly-fitting chunk — blocking alone would NOT have
  fixed that; the loop now re-copies it.
  Gate: `cat /BIG8K.TXT | cat` (~7.8 KiB, 200 lines) asserting >=180 — pre-fix
  ceiling is ~107 at PIPE_SIZE 4096. **Counted, not exact-matched:** kernel prints
  share COM1 and can split a payload line mid-string (measured: 3/500 lost that
  way), so an exact assertion would flake for non-pipe reasons. Local at CI-like
  pacing: **200/200**, head+tail intact, all pipeline regressions PASS, zero
  panics. smoke-shell's 60 s window NOT raised.
- **(previous) LAST_COMPLETED_TASK:** DDR-786 — **multi-stage pipelines `a|b|c`**
  (Section B#12, fifth shell slice). DDR-780 honoured only the FIRST `|`, and its
  right child fell through having already passed the pipe block, so a second `|`
  hit the builtin as a literal token (`cat` tried to open a file named `|`).
  **Answer to the standing question: N stages do NOT force the ~120-line refactor
  DDR-780 deferred** — the fall-through dispatch is reused unchanged, only the
  pipe block grows (~40 lines). The ~10-line design (right child re-scans and
  re-forks) was **REJECTED**: it leaves an intermediate shell at each boundary
  holding the previous read end open, and with non-blocking 4 KiB pipes that
  silently DROPS output instead of hanging — invisible in a small test. Chosen:
  the shell splits all stages up front, forks each itself, threads one pipe
  between neighbours, closes `prev_read`+`fds[1]` in the parent right after each
  fork (DDR-780's EOF lesson, once per boundary), reaps all N. Malformed `|`
  (first/last/doubled) rejected BEFORE any fork. No kernel change.
  **Pre-existing limitation recorded, not introduced:** PIPE_SIZE 4096 +
  non-blocking `pipe_write` ⇒ a stage producing >4 KiB faster than its reader
  drains LOSES data (already true of DDR-780). Blocking pipe writes are a
  kernel-side slice if pipelines are ever used for real streaming.
  Local: 3-stage, 4-stage, malformed rejection, DDR-780/782/784 regressions, and
  shell-still-alive all PASS, zero panics, zero warnings.
- **(previous) LAST_COMPLETED_TASK:** DDR-785 — **`boot_test.sh` early exit**, the
  DDR-783 systemic finding now fixed. The harness always burned the full
  `TIMEOUT_S` then grepped, so the timeout WAS the runtime. Measured: 91
  invocations = **7590 s (126.5 min) of pure waiting per CI run**; the 53 gates
  with no forbidden patterns hold 4050 s of that, ~**43 min/run** of it idle.
  Now: poll the serial capture FILE (QEMU writes a file, not a pipe) and stop the
  guest once `$SENTINEL` + every `EXTRA_SENTINEL` is present; the verification
  block is untouched so verdicts and log-deletion are identical.
  **THE HAZARD IS EXCLUDED BY CONSTRUCTION, NOT MITIGATED:** early exit runs ONLY
  when `FORBIDDEN_SENTINEL` is empty — a forbidden pattern must NOT appear, and
  stopping early would prove only "not yet", so a gate that should FAIL could
  PASS. The 38 forbidden-declaring gates keep today's behaviour byte for byte
  (3540 s left unclaimed on purpose — a guarantee beats an argument). A settle
  window was REJECTED as a heuristic that cannot rule the false-negative out.
  New host-only `make smoke-selftest` (stub qemu, no kernel; wired into CI BEFORE
  the harness judges anything) asserts verdict AND timing — early exit 2 s vs a
  60 s window; **late forbidden pattern still took the full window and FAILED**;
  missing required still failed; declared-but-absent forbidden still passed.
  End-to-end: smoke-fs 30 s (60 s window), smoke-uaccess 4 s (30 s window).
  ✅ **CONFIRMED IN CI: run 30200918063 = 105.8 min vs the 152.3 min baseline
  (run 30193738689) — 46.5 min saved on the same 113-step suite**, matching the
  ~43 min predicted from the timeout-budget measurement. `smoke-selftest` passed
  as CI step 5, before the harness judged any gate.
- ✅ **PROMOTED (2026-07-26): `main` fast-forwarded to `ebd708d`** on green run
  30200918063 (113/113) — carries DDR-784 and DDR-785.
- ⚠️ **Watch for two runs per push:** `gh run list` also shows a **Dependabot
  Updates** workflow that completes fast and green. Do NOT mistake it for CI —
  filter on `workflowName == "pradyos-ci"` (this nearly caused a false "green"
  read this session).
- **(previous) LAST_COMPLETED_TASK:** DDR-784 — PRISM diagnostics on **stderr** +
  **`2>`** (Section B#12, fourth shell slice). **The prerequisite check re-scoped
  it again:** PRISM had ZERO writers to fd 2 (everything went `printf` → fd 1), so
  `2>` alone would have been untestable sugar. Two halves: route genuine errors to
  `fprintf(stderr, …)`, then add `2>`. Success messages stay on stdout —
  **`rm: removed …` is gate-asserted** and moving it would have broken
  `smoke-shell` silently (checked first, per lesson 3). No kernel change (fd 2 is
  already `FD_CONSOLE`; `fd_write_user` is fd-agnostic). **Surfaced only at link
  time:** the musl SUBSET had no `stderr.c`/`fprintf.c` (`undefined symbol:
  stderr`) and no `snprintf` either, so `tools/build_musl.sh` gained those two
  upstream sources — recorded, not folded in quietly. Gate discriminates by
  construction (stdout and stderr to DIFFERENT files in one command, so a broken
  `2>` hides the error in the stdout file). All local checks PASS, zero panics.
- **(previous) LAST_COMPLETED_TASK:** DDR-783 — `smoke-fs` ran at the harness
  default `TIMEOUT_S=30` while asserting the **last sentinel in the whole boot
  chain**. Measured: that sentinel lands at **t=24.26 s** locally (19 % margin),
  so a slower CI runner flakes it — which is what killed run 30192189559 at step
  10. **NOT a DDR-782 regression** (that slice never touches the kernel-internal
  `sfs_selftest_lz4`, and the same image passes locally). `smoke-user` already
  asserts the identical sentinel at 60 s, so this gate was simply never updated as
  the SFS chain grew. Fixed: `TIMEOUT_S=60` on `smoke-fs` alone — the other 56
  default-30 gates assert earlier sentinels and were NOT blind-tuned. Cannot mask
  a hang (`boot_test.sh` greps after the window regardless).
  **Systemic finding, proposal only, NOT applied:** the harness always burns the
  full window instead of exiting once all sentinels are seen — that is why every
  timeout needs hand-tuning and why wall-clock is the sum of timeouts, not of
  work. Early exit would kill this whole flake class AND speed up CI, but it sits
  behind 100+ gates and `FORBIDDEN_SENTINEL` must still fail if the forbidden
  pattern would have appeared after the early exit. Worth its own slice.
- ✅ **DDR-779 / ADR-033 IMPLEMENTED (2026-07-26) — musl mirror, maintainer
  signed off.** The hard stop is cleared. `.gitmodules` now fetches
  `third_party/musl` from `https://github.com/ifduyue/musl`; **the pinned commit
  is byte-identical** and the diff is one URL line. Forced by a **third** outage:
  run **30188805082** (`a077ccd`, DDR-782) died at step 2 `actions/checkout@v5`
  with the identical signature, 3 of 113 steps run — NOT a DDR-782 regression.
  **Two corrections found during verification:** (1) the mirror DDR-779 named,
  `bminor/musl`, **does not exist** (404 — applying the proposal verbatim would
  have broken CI differently); (2) content identity is confirmed by SHA **and
  tree** across three independent mirrors (`ifduyue`, `tianon`, `kraj` — all tree
  `2deb5f7c62d8c9e9733c9ed77d9210b708bbb69e`, equal to the local submodule fetched
  from **upstream**); `EOSIO/musl` and `AssemblyScript/musl` lack the commit and
  were rejected. Principle: the SHA is the contract, the host is availability
  only and swappable in one line. Upstream stays canonical for version bumps.
  **Residual risk:** a third-party mirror could be deleted — that fails loudly,
  not silently. **The proof this worked is a run getting PAST step 2.**
- ✅ **PROMOTED (2026-07-26): `main` fast-forwarded to `721807f`** on CI run
  **30184411583** (conclusion `success`, 112/112 steps, zero failures). That run
  promotes the whole backlog — `21e4a51` (DDR-778 `>`), `d8d78e4` (DDR-779
  finding), `6561a74` (DDR-780 `|`), `721807f` (DDR-781 `>>`/`<`) — and CI-validates
  all four PRISM shell assertion sets, retiring the risk that the gate's tighter
  FIFO pacing (0.5–0.7 s vs the 1–2 s used locally) would drop markers.
- **LAST_COMPLETED_TASK (newest):** DDR-782 kernel `O_TRUNC` + atomic `O_APPEND`
  (Section B#12, kernel-side remainder) — closes the two REAL defects DDR-781
  recorded. **The prerequisite check re-scoped it:** `struct vfs_fs_ops` has **NO
  truncate op** and no FS driver (FAT32/SFS/ext4) can shorten a file, so a literal
  `ftruncate` would be a new VFS op implemented three times — reported instead of
  invented. `O_TRUNC` = `vfs_unlink` + `vfs_create` on an existing file
  (truncation-to-zero, all `>` needs; honest limitation: fresh inode/cookie,
  non-zero `ftruncate` still impossible). `O_APPEND` = fd-layer only, `e->off =
  e->file->size` once per `write()` call in `sys_io.c` before the chunk loop —
  that IS the atomicity property. Flags live in `sys_file.h` (Linux values
  `O_TRUNC 0x200`, `O_APPEND 0x400`) since two TUs honour them. No new syscall, no
  new VFS op, no on-disk change, **no capability change** (CAP_FS_WRITE already
  gates create/unlink/write). PRISM's `>`/`>>` use the flags and the
  `SYS_LSEEK`/`SEEK_END` defines are removed (no dead refs). New `smoke-shell`
  check is discriminating BY CONSTRUCTION — long write then short write, and the
  long record's `TAIL9x3` tail must be ABSENT (it survives under the old
  behaviour). Local: truncate, append, pipe and `<` all PASS, zero panics.
  S2 + S6 apply; no invariant weakened.
- **(previous) LAST_COMPLETED_TASK:** DDR-781 PRISM `<` and `>>` (Section B#12,
  third slice) — ring-3 only. **The prerequisite check changed the mechanism:**
  the kernel has **NO `O_APPEND`** (`sys_file.c` honours only `O_CREAT`), so the
  planned open-flag does not exist; `SYS_LSEEK = 10` DOES support `SEEK_END`, so
  append = `open` + seek-to-EOF in ring 3. Documented as NOT atomic `O_APPEND`
  (fine: one writer per command). Also exposed a PRE-EXISTING gap — there is no
  `O_TRUNC`, so DDR-778's `>` does not truncate; recorded, out of scope.
  All discriminating checks PASS locally (append kept BOTH records; `<` gave the
  marker with no `cat: cannot open <`; pipe regression intact; no panic).
- **NEXT_TASK — B#12 remainder or a new item.** Left in B#12: stderr (`2>`),
  multi-stage pipelines (`a|b|c`), job control (`&`, job table — needs signal
  plumbing). Kernel-side prerequisites for atomic `O_APPEND` / `O_TRUNC` would be
  their own slice touching `sys_file.c` + the VFS write path — scope explicitly,
  do not slip in. Otherwise pick a fresh Section B item, and CHECK THE TREE FIRST
  (#5, #7, #8 were all stale "planned" entries already shipped).
- **(superseded) earlier NEXT_TASK: `|` between two commands.** NOTE (already
  established, don't rediscover): PRISM builtins are INTERNAL FUNCTIONS, not
  execs, so piping needs a fork around the **builtin dispatch** itself. Then `<`,
  `>>`, stderr; job control (`&`, job table) is a separate slice needing signal
  plumbing. Alternatives if B#12 stalls: B#4 SFS-as-default-root (invasive —
  global boot topology), B#6 ext4 write (large). **Check the tree before planning
  ANY Section B item** — #5, #7 and #8 were all stale "planned" entries that were
  already shipped.
- **(superseded) earlier framing: B#12 pipes / redirection / job control** (best remaining
  bounded+gateable item: ring-3 only, `user/prism.c`, zero kernel risk, and the
  kernel side already ships — `SYS_PIPE`, `dup2`, `SIGPIPE`, gates `smoke-syspipe`
  / the CI "pipe/dup2" step). Scope it SMALL in its DDR: start with output
  redirection `>` for one command (uses existing SYS_OPEN + dup2), then `|`
  between two commands via fork+pipe. NOTE PRISM builtins are internal functions,
  not execs, so piping *builtins* needs a fork around the builtin dispatch —
  decide that explicitly in the DDR rather than discovering it mid-implementation.
  Gate: extend `smoke-shell` with a deterministic redirect/pipe assertion.
  Alternatives weighed and rejected for now: B#4 SFS-as-default-root (invasive —
  changes global boot topology, needs broad gate validation), B#6 ext4 write
  (large, ADR-019 extension + journal), B#9 I/O APIC (low priority, replaces 8259).
- **(superseded) earlier framing: read the discriminator, then fix what it names.** Grep every
  future run for `\[hb\] t=`, `user-on-AP probe t=`, and `vblk\] stuck`. Only
  after the mechanism is named should a behavioural fix be written — blind fixes
  are now THREE times refuted (DDR-771 timeout bump; the virtio-blk theory; the
  "timeout ⇒ hang" reading).
- **(superseded) earlier plan: prove or refute the stalled-timer hypothesis.**
  Decisive, cheap experiment: (a) a heartbeat print driven from the timer path in
  `idt.c` (throttled, e.g. every 500 ticks: `[hb] t=<g_ticks> cpu=<id>`), and
  (b) print `g_ticks` at entry and exit of the AP proofs in main.c. If the
  heartbeat STOPS during a failing CI run, the LAPIC/timer path under `-smp 4` is
  the root cause and B#3 moves there entirely. If the heartbeat KEEPS TICKING
  while `smpuser_proof` still prints nothing, the hypothesis is refuted and the
  stall is inside the proof/scheduler instead. Same design rules as DDR-776:
  passive observer, timer-driven, NO locks from the ISR (S6), bounded + print
  throttled (S2), deterministic. Do NOT add another timeout bump and do NOT
  attempt a behavioural fix — blind fixes are now TWICE refuted.
- **(superseded) earlier plan: instrument the AP/scheduler path, NOT another
  blk fix.** Do for the SMP/AP proofs what DDR-776 did for blk: bounded,
  deterministic progress prints so the next failure names WHICH AP proof stalls
  and where. Candidates to instrument: the user-on-AP handoff (`[smp] user on AP
  OK` emitter) and the percpu run-queue/AP wake path. Keep every added wait
  bounded (S2) and every print deterministic; do NOT add another timeout bump.
  Only after the failing step is named should a behavioural fix be attempted —
  blind fixes are now TWICE refuted (timeout bump, then the blk hypothesis).
- **(deferred, still valid on merit) the virtio-blk S2 fix (bound the wait),** FIRST check whether any CI run since DDR-776 printed
  `[vblk] stuck dev=… slot=… lba=… age=…`: if it did, that names the stuck request
  and the trigger (missed IRQ vs lost `virtq_pop_used` vs `head2slot` corruption)
  becomes decidable — root-cause THAT rather than guessing. Then bound
  `submit()`'s completion wait, which per DDR-776 means adding a **scheduler
  timed-block** (`sched_block_on()` has no timeout today) — a genuine scheduler
  feature touching `kernel/proc/sched.{c,h}`, so scope it in its own DDR, do not
  slip it in. On timeout you MUST release the slot (`v->req[s].used = 0`), clear
  `v->req[s].waiter`, and wake any `v->slot_waiter`, or you leak a slot. Also fix
  Hazard 2: `slot_waiter` is a single `struct tcb *`, so >VBLK_NREQ(8) concurrent
  submitters lose a waiter — make it a real wait list or wake-all + re-check.
  Remember: the bug does NOT reproduce locally and is intermittent in CI, so ONE
  green run does not prove a fix — say "not yet proven".
- **(superseded) earlier framing of the B#3 fix slice, per DDR-775:**
  (1) BOUND the wait first (S2): replace the unbounded `sched_block_on` loop with
  a deadline so a missed completion becomes an I/O error + `[vblk] completion
  timeout slot=N lba=…` diagnostic. ⚠ `sched_block_on()` has NO timeout today, so
  this needs either a new scheduler timed-block OR a bounded `g_ticks` yield-loop
  — that choice is the design decision of the slice and must be made explicitly,
  since a yield-loop gives up the deliberate "sleep, never spin" property.
  (2) Then let the diagnostic name the real trigger (missed IRQ vs lost
  `virtq_pop_used` vs `head2slot` corruption) instead of guessing.
  (3) Fix `slot_waiter` into a real wait list (or wake-all + re-check).
- **(superseded framing) Section B#3, the `-smp 4` hang — TOP BLOCKER:** The reproducer requirement is now partly met — a
  concrete CI signature exists (hang after `SYSFSTAT OK`, `-smp 4`, 180 s). Plan:
  (1) try to reproduce locally with `make smoke-surfdestroy` a few times (it has
  historically passed locally, so expect CI-only timing); (2) if it will not
  reproduce, add bounded `g_ticks` deadline polls + progress prints around the
  ring-3 systest sequence *after* `SYSFSTAT` so the next CI failure pinpoints the
  stuck step instead of just ending at a sentinel; (3) DO NOT paper over it with
  another timeout bump — the 180 s bound was already ample. Write the DDR first
  citing Section B#3; S2 (bounded everything) governs any new wait.
- **DEFERRED — DDR-774c phase c-2** (Master doc Section B#1, optional perf).
  Delivery is now PROVEN (`irqs=6`, `PRADYOS_NVME_IRQ_OK` gated), so the
  completion path can finally be converted: make `nvme_submit()` wait on an
  IRQ-set completion flag with a **BOUNDED** spin fallback (S2) so a lost or
  misrouted interrupt degrades to polling instead of hanging the boot.
  ⚠ The real risk in c-2 is **not** the interrupt — it is **CQ head/phase
  ownership (S6)**. Today the polling loop in `nvme_submit()` exclusively owns
  reading the phase bit, advancing `cq_head` and ringing the CQ head doorbell,
  while the ISR only increments a counter. c-2 must transfer that ownership
  atomically, not run both concurrently, or an interrupt arriving mid-poll will
  corrupt an in-flight command. Simplest safe design: ISR sets a per-queue
  "completion pending" flag only; `nvme_submit()` still does all CQ mutation, but
  waits on the flag instead of spinning on the phase bit. Keep `smoke-nvme`'s
  RW_OK / PRP_OK / IRQ_OK all green, and keep the DDR-772 multi-command PRP loop
  correct (it issues several commands per call).
  After c-2, Section B#1 is complete; next candidates are B#3 (`-smp 4` race —
  needs a narrow reproducer first) and B#4 (SFS as default process root —
  invasive, global boot topology).
  Alternatives if 774a is rejected: B#3 `-smp 4` race (needs a narrow reproducer
  first; the DDR-771 timeout bump is mitigation, not root cause) or B#4 SFS as
  default process root (invasive — global boot topology, broad gate validation).
  Section E/F stay gated behind a DDR answering the architecture prerequisite
  checklist; F#68 metric lockbox (= invariant S3) is the highest-priority proposed
  item now that Section D is confirmed built.
- **NEXT_TASK (M2/M3):** the mkfs.sfs storage chain (765-770) is complete —
  NVMe + host mkfs.sfs + kernel reads it + nested dirs + persistent root from a
  build image. Remaining open items (pick per priority): lift `VBLK_MAX` past 4
  (MSI-X vector remap) so the provisioned root can be the DEFAULT alongside ext4
  + retire blk2's dual role; FS write budget (1 MiB per-thread LIFETIME) design
  review; `-smp 4` SMP-race root-cause (timeout bumped as interim mitigation);
  NVMe PRP-list (>page single commands) + NVMe IRQ; mkfs multi-leaf trees (>14
  slots). Consider whether M2 is declarable done.

### 0.-1b PRIOR TRACKER (superseded)

- **LAST_COMPLETED_TASK:** DDR-763 SFS B+tree churn (misdiagnosis corrected) —
  CI-green on `main` at `d7d4123`. 97 gates.
- **CURRENT_ACTIVE_TASK:** DDR-762-v2 SFS free-space reclamation — free-EXTENT-RUN
  allocator (`free_runs[256]`, `alloc_run` first-fit+split, snapshot-guarded
  `free_run` on unlink, write uses `alloc_run(nblocks)`). Gate `smoke-sfs-gc` (300×
  create/write/unlink, incompressible data, budget-refreshed) — verified
  discriminating. Full SFS+SMP regression green. Local green; pushed to
  `dev/phase1`, CI-verifying. 98 gates.
- **RETRACTED FINDING — there was NO SFS B+tree bug.** DDR-763 reproduced the
  prior "bug" with per-return-path instrumentation in `sfs_write`: NONE of the
  `sfs_write` markers fired, so `sfs_write` was never reached. The write failed
  earlier in `vfs_write`: `if (current_thread->fs_write_budget < len) return -1`
  (FS_WRITE_BUDGET_DEFAULT = 1 MiB, `sched.h`). The boot thread writes all ~20
  embedded ELFs to SFS (incl. ~100 KB+ musl daemon/PRISM) + FS self-tests,
  consuming most of its 1 MiB; only ~10 more 64 KB writes fit → fail at ~cycle 11.
  The ~14-slot leaf-split coincidence was a red herring. PROOF: refresh the boot
  thread's budget → churn reaches 40/40, failop=0. `sfs.c` UNCHANGED. B+tree is
  sound.
- **NEXT_TASK:** M2 continues —
    1. **NVMe driver** (registers with the blk layer; needs an NVMe QEMU disk —
       adds boot disk topology). Large new driver.
    2. **host `mkfs.sfs`** — cross-reboot SFS persistence + on-disk free-extent
       tree (the in-memory `free_runs` from DDR-762-v2 is within-a-boot only).
    3. (Design note, revisit before M2 done) the 1 MiB *lifetime* per-thread FS
       write budget (`FS_WRITE_BUDGET_DEFAULT`) is very restrictive for a real
       process writing many files on the persistent SFS root — a separate future
       decision (higher / refillable / per-op), not a bug.
    4. (Open audit) `smoke-percpu-sched` intermittent `-smp 4` early-boot FS flake
       — must be root-caused before M2 is declared complete.
- **AUDIT FINDING (open, recurring low-freq): `-smp 4` one-shot boot-proof
  window flakes.** Two distinct `-smp 4` gates have each failed ONCE on CI, always
  0/3–4 locally, always with the commit only shifting boot timing (never in the
  failing path's logic):
    - `smoke-percpu-sched` (run 29634662558): boot FS self-tests red
      (`/HELLO.TXT not found`, `[sfs] created 0`) — run before any user spawn.
    - `smoke-msixap` (run 29648027891): `[blk] msix on AP OK` not observed under
      DDR-761 (a daemon-config change unrelated to the blk/MSI-X path).
  ROOT PATTERN: these gates assert a one-shot boot proof (`compl_ap` set, per-CPU
  sched OK) by CHECKING ONCE at a fixed boot point; on slow TCG under `-smp 4` the
  awaited cross-CPU event sometimes hasn't landed yet, so the check fails even
  though the mechanism is correct. FIX: poll the condition with a bounded `g_ticks`
  deadline instead of a single check.
    - **`msixap` FIXED (this commit):** the proof now polls
      `virtio_blk_completed_on_ap()` (deadline `g_ticks+200`) while ISSUING blk0
      reads — unit 0's MSI-X vector targets an AP, so the loop forces the very
      completion it waits for. Deterministic; 5/5 local. No kernel-logic change.
    - **STILL OPEN:** `percpu-sched` (different symptom — early block I/O
      intermittently failing, not a check-once proof; needs its own root-cause)
      and `smpjob` (already has a per-AP deadline; lower risk). Later audit slice.
- **Milestone track:** M1 kernel hardening (per the revised master prompt §11);
  then M2 storage (persistent SFS root half-2/2, GC, NVMe).

### 0.0.1 STATE DELTA (2026-07-15 — DDR-743..756, supersedes §0.0 below)

- **HEAD:** `69d3474` (`main` == `dev/phase1`, both pushed, CI-green). **92 gates.**
- Shipped since §0.0 (each DDR-first + CI-green before main ff):
  **743** SYS_GETPROCS/`ps` · **744** ring-3 file lifecycle (O_CREAT + SYS_UNLINK
  + the FD_VFS write path, which was a stub) · **745** PRISM touch/rm ·
  **746** ACPI S5 poweroff (SYS_POWEROFF 69, CAP_SOVEREIGN; compositor `p`) ·
  **747** ACPI reboot (SYS_REBOOT 70; FADT reset reg + 0xCF9 + 8042; `b`) ·
  **748** SYS_SYSINFO 71 (CPUID vendor/brand, cpus, uptime, free pages) ·
  **749** SYS_TIME 72 (RTC broken-down) · **750** klog ring + SYS_DMESG 73 ·
  **751** PRISM uname/date/uptime/dmesg · **752** SYS_MEMINFO 74 + PMM
  total-RAM tracking + PRISM free · **753** TCP loopback echo self-test
  (client path; `smoke-net-tcp-lo`) · **754** ps CPU accounting (procinfo grew
  run_ticks/dispatches) · **755** kill end-to-end (`smoke-kill`) + PRISM kill ·
  **756** SYS_SETNAME 75 (tcb.name_buf; `smoke-setname`).
- **NSI extends through 75.** PRISM builtins: help echo cat run ls ps kill
  setname touch rm uname date uptime dmesg free mode exit.
- Known env quirks: WSL pipe exit codes flake (route output to build/*.log);
  boot_test deletes its serial log (use explicit qemu run to inspect output);
  `ls`-gate prompt-prefix flake fixed in 743 (`(^|prism> )` anchor pattern).

### 0.0 CURRENT STATE (updated 2026-07-04 — superseded by §0.0.1 above)

- **HEAD:** `59d7ac4` (`main` == `dev/phase1`, both pushed). **CI: all 57 gates
  green** (run 28683624946). Git runs from native Windows (PowerShell); builds
  in WSL **Ubuntu-24.04**. `third_party/{musl,lwip}` are submodules — run
  `git submodule update --init --recursive` after a fresh clone/hard reset.
- **SMP track (ADR-030, staged — 5 slices CI-green after the 52-gate state):**
  stage 1 PMM/kheap/console under spinlocks (`smoke-smplock`); stage 2 percpu
  identity (`smoke-percpu`); stage 3a **SWAPGS discipline** on all 4 ring
  transitions + `this_cpu()` = `%gs:0` (`smoke-swapgs`); stage 3b
  `current_thread`@`%gs:8` + SYSCALL kstack@`%gs:16` — `current_thread` is a
  sched.h macro, `percpu_init_early` runs right after `gdt_init` (its gs reload
  zeroes the base!) (`smoke-percpu-sched`); stage 3c-alpha **AP work dispatch**
  — wake IPI vector 49 + single-slot percpu mailbox, `smp_run_on`/
  `smp_job_done`; APs `idt_load_ap()` before sti (real-mode IDTR leftover
  triple-faulted otherwise) (`smoke-smpjob`).
- **3c-locks-1 shipped (`smoke-crosswake`):** the scheduler runs under
  `g_sched_lock` (held across `context_switch`; a NEW thread's first entry
  releases it in `thread_trampoline` — a leak would deadlock); `sched_unblock`
  is an atomic CAS callable from APs. **Loader contract changed
  (DDR-boot-authority-race, fixed the 3× `smoke-agents` CI flake):**
  `elf_load` returns threads **BLOCKED** — callers set `is_sovereign`/`is_agent`
  and then `sched_unblock` (see `elf.h`; `user_boot_from_sfs` gained a
  `sovereign` param). HEAD is now `e5e9f56`, 58 gates green (run 28699475194).
- **Full 3c is DONE — ADR-031 complete (2026-07-05, HEAD `62b9a97`, 61 gates).**
  The lock slices (locks-2 block sleep-mutex, locks-3 per-mount VFS, locks-4
  IPC/bcast + `sched_block_on`) then the capstone: cap-1 per-CPU TSS/GDT/TR,
  cap-2a SMP-safe ring (`on_cpu` claim, locked topology, BLOCKED-create),
  cap-2b APs run kernel threads (per-CPU idle — AP idles kmalloc'd, not BSS;
  `g_sched_ready`; `smoke-smpsched`), cap-3 per-AP LAPIC-timer preemption
  (global tick side-effects BSP-only or `g_ticks` runs Nx fast;
  `smoke-smppreempt`), cap-4 ring 3 on every core (per-CPU SYSCALL snapshot in
  percpu @56..120; per-AP `cpu_enable_sse`/NXE/SYSCALL-MSRs — see memory
  `ap-percpu-machine-state`; `smoke-smpuser`). ADR-016 fully superseded for the
  scheduler. Still deferred: IDT gate for the spurious vector (0xFF), per-CPU
  runqueues/affinity.
- **DDR-714 stage C is DONE (2026-07-06, HEAD `ea7977f`, 62 gates):** C1/C2 =
  MSI-X for ALL virtio devices (blk vectors 50..53, net 54 both queues, input
  55; `virtio_pci_msix_setup`, IDT→56 stubs, `msix_register` + LAPIC-only EOI;
  INTx fallback kept; engagement pinned in smoke-fs/smoke-net-lo). The 8259
  now carries ONLY ISA (keyboard IRQ1, COM1 RX IRQ4) — I/O APIC deliberately
  skipped (q35 PIC-mode Interrupt Line ≠ IOAPIC GSI; MSI-X sidesteps it). C3 =
  blk vectors distributed round-robin across APs; the locks-2 completion review
  done (per-device `compl_lock` + `sched_block_on` closes the cross-CPU lost
  wakeup; net/input stay BSP-routed — lwIP is single-threaded by design). Gate
  `smoke-msixap`. Also fixed en route (CI-caught): the exit-vs-collect kstack
  use-after-free (`sched_exit` now holds `g_sched_lock` ACROSS its final
  switch — DDR-SMP-exit-stack-race).
- **Multi-in-flight blk is DONE (DDR-BLK-1, HEAD `e0c8f79`, 63 gates):** the
  one-in-flight `busy` mutex is deleted — 8 per-request slots per disk
  (`head2slot[]`), ALL vq+slot state under `compl_lock`, slot exhaustion
  sleeps via `sched_block_on`. Gate `smoke-blkmq`.
- **Per-CPU runqueues are DONE (DDR-SMP-rq-1, 64 gates):** O(1) pick from
  per-CPU FIFOs + trylock work stealing; the ring is topology only. Bugs found:
  idle starvation (rotate through the own idle on empty queues); COMPOSIT now
  spawns before SURFTEST (client outraced compositor init). `smoke-rqstress`.
- **`g_sched_lock` is OFF the switch path (DDR-SMP-rq-2, HEAD `176e329`,
  73 gates):** the hot path takes only local IRQ masking + its own rq leaf
  lock. The lock's two real jobs became an explicit **off-CPU handshake**:
  `on_cpu >= 0` = "still executing / rsp not saved", release-stored AFTER the
  switch by whoever resumes on that CPU (`finish_task_switch` reading the new
  `percpu.prev`); pickers `switch_wait_offcpu()` (acquire-spin) before touching
  `next->rsp`; `sched_free_tcb` waits on it, so `sched_exit` no longer holds the
  lock across its final switch (exit-stack-race guarantee preserved, mechanism
  local). `rq_take` dropped `on_cpu` from its filter — the DEQUEUE is the
  exclusion — which RETIRED rq-1's transient re-append. `g_sched_lock` now
  covers ring topology only. `percpu.prev` must stay AFTER the asm-consumed
  `u_*` block (static asserts enforce).
- **Per-wake reschedule IPIs are DONE (DDR-SMP-rq-3, 74 gates):** rq-2's
  deferred half. `sched_unblock` now finds an idle CPU (`percpu.idle`) and sends
  a directed `smp_resched_one` wake IPI (vector-49 wake ISR) so the woken thread
  is stolen immediately instead of on the AP's next 100 Hz tick. The wake/halt
  race is closed by the idle loop's double-check (set `idle=1`, re-scan
  `rq_has_ready()`, loop before `hlt`), timer as backstop. Gate `smoke-resched`
  (`g_resched_ipis > 0`). **Fixed a latent bug rq-3 exposed:** `sys_write`'s
  console path emitted user bytes via a bare per-char `kputc` loop with no lock,
  so a ring-3 printer on an AP garbled mid-line against another CPU's `kputs`.
  Root-caused with `kwrite(buf,n)` in `console.c` (takes `g_console_lock` once
  per chunk → user writes get `kputs`'s line-atomicity). `u_*` static asserts
  unaffected (`idle` appended after `prev`).
- **L7 polish resumed:** **DDR-720** Tab window cycling (compositor hotkey,
  `smoke-alttab`), **DDR-721** double-buffered page flip (two host GPU
  resources over one guest buffer; flush = transfer-offscreen → scanout-flip;
  `smoke-flip`), **DDR-722** real glass blur + saturation (separable box blur
  under cards, tint BLENDS over — an opaque fill erases the blur;
  `smoke-glassblur`), **DDR-723** multi-stop gradient backdrops
  (`smoke-gradient`), **DDR-724** window decorations — focus-colored frame +
  drop shadow (`smoke-decor`). HEAD `a184120`, **69 gates**, all CI-green.
  **DDR-725** scroll-wheel plumbing (REL_WHEEL → `mouse_state.wheel` →
  type-2 surface event to focus; `wheel_inject.sh`; `smoke-scroll`).
  **DDR-726** auto ambiance cadence + pre-transition pulse (frame-loop time
  base, `k` test knob, `smoke-cadence`). **DDR-727** spring toggle + click
  ripple (`smoke-motion`; `smoke-mouse` also asserts the ripple). **DDR-728**
  the Inter typeface as a 16px alpha glyph ATLAS — `tools/fontgen/gen_inter.c`
  + vendored public-domain `stb_truetype.h` rasterize Inter-Regular (SIL OFL)
  into the generated, committed `user/inter_font.h` (~22 KB); the TTF is NOT
  vendored (rendered bitmaps aren't font software; the no-out-of-tree-libs wall
  governs the OS IMAGE, not build-host tooling). Proportional titles via
  `draw_str_inter`; 8×8 face kept for small labels. `smoke-font`.
  HEAD `aa7f548`, **73 gates**, all CI-green.
- **The DDR-702..709 deferred VISUAL list is CLOSED** — glass blur, gradients,
  typeface, cadence + pre-transition, spring/ripple, page-flip, scroll,
  decorations, alt-tab all shipped.
- **Surface destroy is DONE (DDR-729, 75 gates):** completes the surface
  lifecycle. Root-fixed ownership: client/compositor mappings are now
  `PTE_SW_SHARED` views (vDSO precedent), so `vmm_destroy_address_space` never
  frees surface/FB frames — the surface layer is the sole owner. `sched_exit`
  calls `surface_reap_pid()` so a client that dies without `SYS_SURFACE_CLOSE`
  no longer leaks its 16-slot table entry + frames (previously `-EMFILE` after a
  few create/exit cycles; also closed a latent double-free vs. AS teardown and on
  sovereign-close). A `g_surf_lock` leaf spinlock makes the slot lifecycle
  SMP-safe. Gate `smoke-surfdestroy` (`-smp 4`, freestanding
  `user/surfdestroytest.c` — musl-free to stay inside the 512 KiB kernel-image
  budget) proves churn + slot-reuse + exit-reclamation. Also fixed the identical
  missing-shared-bit flaw in `sys_fb.c`.
- **Per-agent live metrics are DONE (DDR-730, 76 gates):** roster slots are now
  `{used, pid, actions}`; "active" is DERIVED (slot pid resolves to a live agent
  tcb), root-fixing DDR-707's never-cleared active bit — dead agents' cards
  self-dim with no teardown hook. New `SYS_AGENT_METRICS` (NSI 64;
  `MAX_SYSCALLS` 64->80) returns `{pid, state, mem_used, actions}` live from the
  tcb; `SYS_AGENT_ROSTER` shares the liveness check. Witness = freestanding
  `user/agentmetricstest.c` probe (gate `smoke-agentmetrics`); the compositor is
  untouched (its musl ELF had no image headroom).
  **Kernel load window is at its real-mode CEILING: 544 KiB** (stage2 17x64
  sectors, ends 0x98000 < conventional-RAM top 0x9FC00). A 24-chunk/768 KiB
  attempt hung the boot — the 0xA0000 VGA/ROM hole makes a bigger flat load at
  0x10000 impossible. Past 544 KiB the kernel must be relocated above 1 MiB
  (unreal-mode bounce copy in stage2) — plan it as a dedicated boot slice.
- **CAP_NET is DONE (DDR-731, 77 gates):** the socket NSI is gated. New
  `tcb.is_net` (granted to agents at spawn; zeroed in `sched_create_state`, not
  fork-inherited); `SYS_SOCK_CONNECT` needs `is_net || is_sovereign` (audited
  `-EPERM`); per-slot `g_sock_owner` enforced on WRITE/READ/CLOSE (no
  cross-process hijack); `socket_reap_pid` from `sched_exit` (DDR-729 pattern).
  Gate `smoke-capnet` (freestanding `user/capnettest.c`).
- **Kernel relocated to 4 MiB (DDR-733, 77 gates):** the flat load at 0x10000
  hit its ~575 KiB file+BSS ceiling (BSS tail crossed 0x9FC00 into the EBDA →
  #GP on the first tick, gate-caught). Stage2 now INT13-reads into a 0x10000
  bounce buffer and unreal-mode-copies each chunk to `KERNEL_PHYS = 0x400000`
  (SDM Vol.3 §9.9.2; DS/ES limits re-armed per chunk under cli). 24-chunk /
  768 KiB read window; runtime ceiling = the 2 MiB PT_HI span, enforced by an
  nm-based `__bss_end` Makefile check. kernel.ld `KERNEL_LMA = 0x400000`.
- **AETHER boot config is DONE (DDR-732, 78 gates):** the daemon reads
  `/AETHER.CFG` (FAT32 boot volume, mcopy'd at image build) for mode/task/slot;
  compiled defaults + `PRADYOS_AETHER_CFG_DEFAULT` on any config problem. Gate
  `smoke-aethercfg`. (Not SFS: ring 3's root_mnt is the FAT volume; renaming to
  /etc/aether/config awaits SFS-as-process-root.)
  **ALL deferred non-visual L7 items are now CLOSED.**
- **CAP_NET allowlist is DONE (DDR-734, 79 gates) — live-agent hardening 1/3:**
  deny-by-default egress for CAP_NET agents. Bounded kernel list ({host_be,
  port}, port 0 = any), consulted in `sys_sock_connect` (sovereign bypasses);
  sovereign-only install NSI `SYS_NET_ALLOW` (65, install-only — no revocation
  surface); daemon installs `net=<ip>:<port>` rows from `/AETHER.CFG` before
  any agent spawns (default `net=10.0.2.2:11434` keeps smoke-agent-live
  working). Gate `smoke-netallow`.
- **Agent CPU metric is DONE (DDR-735, 79 gates) — live-agent hardening 2/3:**
  `SYS_AGENT_METRICS` gains `run_ticks` (from `sched_tick`) + `dispatches` (from
  `schedule()`), owner-CPU-written (no hot-path lock). The roster slot retains
  the counts past agent exit so a short-lived agent is provable; the probe
  latches "alive" and "dispatched" independently. Extended `smoke-agentmetrics`.
  Also: `boot_test.sh` `SERIAL_LOG` is now overridable (this WSL wipes /tmp
  mid-run — set SERIAL_LOG to a persistent path for reliable local gates; CI
  default unchanged). Hardening 3/3 = draw the counts on the agent cards.
- **rq double-enqueue FIXED (DDR-736):** the DDR-735-era CI failures (kfree
  double-free; smpsched hang) had one root cause — `rq_push`'s `rq_on` check
  ran under the target queue's lock, so a waker's unblock and the blocker's own
  schedule() re-queue (two DIFFERENT leaf locks) could link one tcb into TWO
  FIFOs via its single `rq_next` (list corruption = hang; double-pop/double-run
  = double free). The `rq_on` claim is now an atomic exchange BEFORE any queue
  lock; `rq_take` clears it with RELEASE. Also hardened `thread_trampoline`
  (keeps `on_cpu` set until `finish_task_switch`, matching sched_exit) and the
  KASAN double-free panic now prints ptr+objsize. Race predates DDR-735; CI's
  TCG runners surfaced it. LOCAL GATE NOTE: back-to-back local runs flake on a
  QEMU image-lock release race + this WSL wipes /tmp (use SERIAL_LOG=<persistent
  path>, sleep 1 between gates); CI (isolated steps) is authoritative.
- **smoke-agentmetrics made TCG-deterministic (DDR-735 gate fix):** the gate's
  alive-window assertion was racy on TCG (an agent's whole life fits inside one
  slow compositor frame). `sched_exit` now captures final counters into the
  roster slot (`agent_metrics_reap`) and retains the pid; the probe asserts
  post-mortem facts (`pid!=0 && dispatches>=1` vs empty slot 7), RTC-bounded.
- **Agent-panel metrics are DONE (DDR-737, 80 gates) — hardening campaign
  CLOSED (1/3 CAP_NET allowlist, 2/3 CPU metrics, 3/3 panel UI):** cards render
  from `SYS_AGENT_METRICS` — state-colored dot (dim green = ran-and-exited via
  retained pid) + up to 4 action pips; one-shot `AGENT_PANEL KRYOS` witness on
  the post-mortem-stable fact. Gate `smoke-agentpanel` (GPU). The DDR-730
  image-budget blocker is gone (DDR-733: kernel 545 KiB of 768).
- **SFS hierarchical dirs are DONE (DDR-738, 81 gates):** path walk +
  `SFS_INO_DIR` inode flag (root implicitly a dir) + mkdir-p on create; `open`
  requires dir intermediates, `readdir` walks to the target dir. SFS-local (VFS
  vtable unchanged; process root still FAT). Gate `smoke-sfs-dirs` builds
  `/etc/aether/config`. FOLLOW-ONS: (a) switch a process's root_mnt to SFS
  (needs image-time SFS tree provisioning — the kernel currently writes SFS
  files at boot, there is no host mkfs.sfs); (b) then move /AETHER.CFG ->
  /etc/aether/config. rmdir/unlink-dir, `.`/`..`, hard links still absent.
- **Per-process root mount is DONE (DDR-739, 82 gates) — SFS-as-root half 1/2:**
  a process can be spawned with a chosen root_mnt (spawner sets t->root_mnt
  before sched_unblock; fork inherits). Proven by an ext4-rooted ring-3 probe
  (user/rootmounttest.c) opening /EXT4.TXT + failing /HELLO.TXT. ext4 (blk3) is
  mounted ONCE early in kmain and reused by the ext4 self-test. Gate
  smoke-rootmount (needs ext4 disk). HALF 2/2 (the actual SFS root) still needs a
  PERSISTENT SFS volume — the destructive SFS self-tests reformat the only SFS
  disk — plus image-time /etc/aether/config provisioning.
- **Context-switch lazy FPU is DONE (DDR-740):** schedule() only fxsave/fxrstor's
  the FPU across USER threads now (guarded on is_user) — kernel threads are
  -mgeneral-regs-only and never touch it. Switch cost ~1881 -> ~1054 ns (44%),
  under the <=1500 ns target. smoke-fpu is the correctness gate; the perf number
  is real-hardware (not CI-assertable on TCG). No new gate.
- **SFS unlink+rmdir are DONE (DDR-741, 83 gates):** removal tombstones the DIR
  entry (inode_num==0) since the B+tree has no delete and bt_insert replaces;
  lookup/dir_walk/create honor tombstones (re-creatable, readdir-invisible);
  sfs_unlink does files + empty dirs via the existing vfs_unlink op. Block
  reclamation deferred (bounded leak until reformat). Gate smoke-sfs-unlink.
- **SYS_GETDENTS is DONE (DDR-742, 83 gates):** ring-3 directory listing (NSI 66:
  path,index,name_buf -> namelen|0|-errno), resolves against the caller's
  root_mnt+fs_cap (per-process-root aware). PRISM `ls [dir]` now works; gate is
  smoke-shell extended with `ls /` asserting `^HELLO.TXT$` (bare name, distinct
  from the kernel's boot fs_list). `ps` still a stub (needs a process-table
  syscall). Remaining wall: wlroots/Wayland (out-of-tree).
- **Shipped since `199a637` (each CI-green, DDR/ADR before code):**
  - **DDR-711** window close+resize (`SYS_SURFACE_CLOSE/RESIZE` 59/60, `smoke-winops`).
  - **DDR-712** glass panels + particle field (`blend_px`, `smoke-visual`).
  - **DDR-713** agent-card click → `SYS_SPAWN_AGENT` (`smoke-agent-click`).
    **Root-cause kernel fix:** the AETHER spawn hook is registered BEFORE the
    first user process (boot race — user threads run while kmain boots).
  - **DDR-714 stage A** LAPIC + APIC timer own the 100 Hz tick on vector 48,
    PIT masked, 8259 kept for device IRQs (`kernel/apic/lapic.c`, `smoke-apic`).
  - **DDR-714 stage B / ADR-029** SMP bring-up: INIT-SIPI trampoline at 0x8000
    (`arch/x86_64/ap_boot.asm`), APs online **parked**, spinlock primitive
    (`kernel/include/spinlock.h`), `QEMU_SMP` runner knob (`smoke-smp` -smp 4).
  - **DDR-715** window titles + close button (`SYS_SURFACE_SET_TITLE` 61,
    `smoke-wmclose`); **DDR-716** ambiance backdrops (DAY mesh / DUSK sun-bloom /
    NIGHT nebulas, settled-frame guard, `smoke-backdrop`); **DDR-717** minimize
    + `r` restore (compositor-local, `smoke-wmmin`); **DDR-718** surface event
    channel (`SENDEV/GETEV` 62/63, corner drag-resize, owner-honored,
    `smoke-evresize`); **DDR-719** maximize + geometry restore (+ kernel
    `SURF_POS_KEEP` commit sentinel, `smoke-wmmax`).
  - CI robustness: `smoke-agents` TIMEOUT_S 90→150 (flaked twice on GitHub).
- **NSI now extends through 63.** Window management is feature-complete
  (title/drag/min/max/resize/close). The remaining big tracks: **distributed SMP
  scheduling** (per-CPU + subsystem locking, its own ADR superseding ADR-016),
  **I/O APIC + MSI-X** (DDR-714 stage C), **real glass blur/gradients**.
- Gate wall-clock: ~70 min on CI; locally run the batches from
  `docs/build_status.md`'s gate list (background tasks cap at 10 min — split
  into <10-min chunks, and kill orphaned WSL qemu/make between failed runs).

### 0.1 OLDER STATE (2026-06-28) + resume plan — historical

- **HEAD:** `5e5fb78` (`main` == `dev/phase1`, both pushed; this worktree branch is
  `claude/pedantic-shirley-a27bf3` — see §7). Since the old 5d/5e state (`9f310da`)
  the following shipped, **each CI-green**: **NET-B** (lwIP TCP/IP over virtio-net),
  **Layer 6 AETHER** (agent queue/audit/mem/rate + daemon + agent template),
  **ring-3 socket NSI** (ADR-027, live Ollama path), and **Layer 7** so far —
  mode binding (DDR-701), VirtIO-GPU framebuffer (ADR-028, slice 0), ring-3
  framebuffer surface (DDR-702), and PS/2 keyboard input (DDR-703).
- **CI: all 35 gates green at HEAD `5e5fb78`.** New since 5d/5e: `smoke-net`,
  `smoke-net-lo`, `smoke-net-fuzz`, `smoke-aether`, `smoke-aether-queue`,
  `smoke-aether-sec`, `smoke-mode`, `smoke-gpu`, `smoke-fb`, `smoke-input`
  (+ the dev-only `smoke-agent-live`, not in CI). NSI now extends through 46
  (`SYS_INPUT_POLL`); see `kernel/syscall/syscall.h`.
- **Build distro:** **Ubuntu-24.04** (NOT 22.04 — 22.04 is gone; `sudo` needs a
  password now: the WSL password is the user's to supply).
- **Toolchain:** `llvm-objcopy` symlinked to `llvm-objcopy-18`; `clang-18`/
  `ld.lld-18`/qemu/mkfs.fat/mcopy/mkfs.ext4/nasm/rustup/**python3** present
  (python3 is used by the `smoke-input` keyboard gate).
- **Git note:** the worktree gitdir is a Windows path, so **git runs from native
  Windows (PowerShell)**, not WSL (WSL git errors on the path). Build/test in WSL.
- **Stored remote PAT in `.git/config` is EXPIRED** — push with a fresh token
  inline in the URL (never write it to a tracked file).
- **Boot-memory layout note:** the boot page tables now live at **0x300000** (moved
  off 0x70000 once the kernel image+BSS grew); Stage 2 loads **16×64 sectors
  (512 KiB)**; the 1 MiB disk image accommodates it.

**Completed (PROC-D — musl libc — is COMPLETE):**
- **Layer 5b + IMP/PROC/NET series** were already complete on entry (the old
  handoff was stale — claimed HEAD `4608e9b`/"5b not started"). Reconciled.
- **PROC-D Step 1** (`f2bd207`): `SYS_SET_TLS`=27 (FS-base thread pointer,
  user-range-validated, restored on switch-in from `tcb.fs_base`, fork-inherited),
  `SYS_WRITEV`=28 (iovec gather-write via the validated copyin path; shared
  `fd_write_user`), `EXEC_MAX` 8 KiB→**256 KiB** (PMM-pool buffer). Probe
  `user/tlstest.asm` (`PRADYOS_TLS_OK WRITEV_OK`).
- **PROC-D Steps 2+3** (`8dd2162`, `0cfd957`): musl **v1.2.5** (`third_party/musl`,
  commit `0784374d`) builds to `build/musl/lib/{libc.a,crt1.o}` via `make musl` /
  `tools/build_musl.sh` + `third_party/musl-overlay/`. `user/cmusl.c` is the first
  ring-3 C program — links static against the subset with `user/user_c.ld`, runs
  from SFS, prints `PRADYOS_MUSL_OK v1.2.5 2026` via `printf`→`SYS_WRITEV`.
  `cpu_enable_sse()` enables x87/SSE for the varargs ABI. Design + the hard-won
  details are in **ADR-023** (§D1–D8).

**musl usage for 5d/5e (how to build a ring-3 C program):**
- Compile: `clang --target=x86_64-elf -ffreestanding -fno-pic -fno-pie
  -mcmodel=large -nostdinc -nostdlib -Wall -Wextra -Werror
  -Ibuild/musl/include -Ithird_party/musl/arch/x86_64
  -Ithird_party/musl/arch/generic -Ithird_party/musl/include -c foo.c -o foo.o`
  (**`-mcmodel=large` is mandatory** — the 0x8000000000 base exceeds 32-bit relocs).
- Link: `ld.lld -nostdlib -static -no-pie -T user/user_c.ld
  build/musl/lib/crt1.o foo.o build/musl/lib/libc.a -o foo.elf`.
- Embed like `cmusl` (incbin in `arch/x86_64/user_image.asm` →
  `user_boot_from_sfs` in `kernel/main.c`), or place on FAT/SFS for execve.
- If a libc call hits an undefined symbol at link, add its musl source file to the
  `SRCS` list in `tools/build_musl.sh` (the subset is link-resolved, not exhaustive).
- A new syscall musl needs that the NSI lacks: add it to the NSI (append-only) and
  remap it in `third_party/musl-overlay/syscall_overrides.h`.

**⚠️ BEFORE 5e (or any two concurrent ring-3 C/SSE processes):** add per-thread
`FXSAVE`/`FXRSTOR` to the context switch (512-byte 16-aligned area in the TCB,
saved/restored in `schedule()` like `fs_base`). Today FPU/XMM state is NOT saved
across switches — correct only while one thread uses the FPU at a time (**ADR-023
§D8**, binding trigger). 5d (single PID-1 process) is still safe.

**Next build order:** **5d pradyos-init (PID 1 + orphan reaper)** → 5e PRISM shell
→ NET-B lwIP → Layer 6 AETHER. ADR/DDR before each slice's code.

---

## 1. PROJECT IDENTITY

- **Name:** PRADYOS — Sovereign Edition (kernel: **NEXUS**).
- **Purpose / one-liner:** a from-scratch, bare-metal, AI-native operating system
  with an original x86_64 kernel; built in strict layer/slice order — *a slice
  ships when it is correct, not when it is fast.*
- **Active branch:** `dev/phase1` (fast-forwarded into `main` per slice).
- **Current HEAD:** `5e5fb78` (`main` == `dev/phase1`). **§0.1 is authoritative for
  current state.** The §2 layer-by-layer list below predates the NET-B / Layer 6 /
  Layer 7 work and is kept only for the L1–L5 history; see §0.1 + `docs/build_status.md`
  for everything after PROC-D (NET-B, AETHER, socket NSI, GPU FB, FB surface, input).
- **Repo path (this machine):**
  - Windows: `C:\Users\prady\Documents\Claude\Projects\Prady4OS`
  - WSL: `/mnt/c/Users/prady/Documents/Claude/Projects/Prady4OS`
- **Remote:** `https://github.com/prady4the4bady/Prady4OS.git` (a PAT lives only in
  the local `.git/config`; **never** write it to a tracked file. On a new account,
  configure your own credentials; do not reuse a leaked token.)

---

## 2. CURRENT STATE — LAYER BY LAYER

### ✅ Complete & CI-verified
- **L1 Boot** — MBR two-stage loader (`boot/mbr`, `boot/stage2`) → long mode → ring-0 C. Gate: `smoke`.
- **L2 NEXUS kernel core** — GDT/IDT + CPU-exception panic path; 8259 PIC + 8254 PIT @100 Hz; buddy PMM (ADR-003); SLAB heap; higher-half VMM (ADR-007); round-robin preemptive scheduler (ADR-008); NCS capability system (ADR-009); NIA IPC sync/async/broadcast (ADR-010/011); syscalls + ring 3 via SYSCALL/SYSRET + TSS (ADR-012); preemption-safe shared state (ADR-016). Gate: `smoke`.
- **L3 Drivers** — ACPI + PCIe ECAM (ADR-013); reusable virtio 1.0 transport + generic block layer + interrupt-driven virtio-blk (ADR-014); CMOS RTC (ADR-020). Gates: covered by FS gates.
- **L4 VFS + filesystems** — mount table + per-mount context (ADR-015/017); **FAT32 read-write** + VFAT long-name read + RTC timestamps (ADR-015/020); **SFS** inode CoW B+tree, file extents, journal + atomic tx + crash replay, snapshots, per-extent LZ4, 4 KiB tags (ADR-018); **ext4 read-only** (ADR-019). Gates: `smoke-fs`, `smoke-fs-rw`, `smoke-fs-sfs-rw`, `smoke-fs-ext4`.
- **L5a Userspace — static ELF loader + W^X (ADR-021)** — per-process address spaces + per-process CR3; `EFER.NXE` (CPUID-gated); ELF64 loader maps `PT_LOAD` with `p_flags`→W^X perms; 8 MiB RW+NX stack + guard page; SysV `argc/argv/envp/auxv`; ring-3 entry; user fault → clean kill. Test ELF written to SFS and **loaded back from SFS**. Gate: `smoke-user`.

CI status (this §2 list covers L1–L5a only — the historical baseline). **Current
CI is 35 gates green at HEAD `5e5fb78`** (see §0.1 for the full list); the L1–L5a
gates named here remain part of that set, plus the NET-B / Layer 6 / Layer 7 gates.
**Re-verify the current run yourself before trusting this** (see §9).

### 🟡 In progress / partial
- **L5 Userspace** — only slice **5a** done. Slices 5b (syscalls), 5c (musl),
  5d (pradyos-init PID 1), 5e (PRISM shell), 5f (prad) are **pending**. Next = 5b.

### ⏸ Deferred (tracked, with governing ADR)
- **Kernel-self W^X** (kernel text RX / kernel data NX) — the bootloader maps the
  kernel image RWX; only *user* W^X is enforced today. Governed by **ADR-021**;
  build before running untrusted code in kernel space (kernel-hardening pass).
- **COW fork** — interim will be copy-all-pages (5b); COW later. Record in **DDR-5b**.
- **Process reaping / AS teardown on exit** — `vmm_destroy_address_space` exists and
  is used on load-failure paths, but an exited user process leaks its AS + kstack
  until a reaper. Build with the PID-1 orphan reaper (slice **5d**).
- **Dynamic linking** (`ld-pradyos.so`) — static ELF only for now.
- **APIC / SMP** — legacy PIC+PIT only (**ADR-006**).
- **3-lane NAS scheduler** — round-robin placeholder (**ADR-008**).
- **PMM variable-weight / predictive** — buddy interim (**ADR-003**, OPEN).
- **ext4 write**, **SFS free-space tree / snapshot GC** — read/feature scope set by **ADR-018/019**.
- Full deferred list with "build-before" triggers: `docs/build_status.md` → DEFERRED.

---

## 3. ARCHITECTURE CONTRACTS THAT MUST NEVER BE BROKEN

These are **binding**. ADR-021 is a binding security ADR and may only be changed by
a new superseding ADR — never quietly amended.

1. **W^X invariant (ADR-021).** No page is ever simultaneously writable and
   executable, in any address space, anywhere in Layer 5.
   - text/`PF_X` → present, USER, **RX** (no W, no NX).
   - rodata/`PF_R` → present, USER, **R + NX**.
   - data/BSS/`PF_W`, heap, user stack → present, USER, **RW + NX**.
   - guard page → **not present** (PTE = 0), placed immediately below the stack.
   - A segment that is both `PF_W` and `PF_X` is **rejected** (`ELF_E_WX`), never mapped RWX.
   - Enforced in `kernel/exec/elf.c` (flag derivation) + `kernel/mm/vmm.c`
     (`map_core` writes PTE bit 63 = NX). **What breaks it:** mapping any user page
     with both `VMM_RW` and without `VMM_NX`; reusing the deleted RWX `user_demo`;
     dropping the W+X rejection. Negative regression `user/wxviol.asm` must keep
     producing `#PF err=0x7 → clean kill`.
2. **NX gating.** `EFER.NXE` is enabled **only if** `CPUID.8000_0001h:EDX[20]`
   reports NX (`g_nx_ok` in `kernel/mm/vmm.c`). When `g_nx_ok` is false, `map_core`
   must **not** set PTE bit 63 (setting it with NXE clear faults). Never force NXE
   unconditionally.
3. **User-fault isolation (ADR-021).** A fault from **CPL 3** (`isr_dispatch`,
   `kernel/idt.c`) must become a clean process kill via `sched_exit` with a
   `[trap] user …` line — **never** a kernel panic. CPL-0 faults still panic.
4. **Root-cause-only fixes.** No patchwork, no warning suppression, no masking
   symptoms. If a design issue appears, write/adjust an ADR or DDR first.
5. **`-Werror` must stay green at all times** — clang **and** nasm
   (`NASM_WERROR := -Werror`). Any warning is a build failure.
6. **Repo stays clean & organized.** No `TODO`/`FIXME`, no dead code/refs, no new
   flat files in `kernel/` root (use subsystem subdirs). `docs/build_status.md` is
   updated in the **same commit** as the code it describes; keep
   `docs/platform_profiles.md` accurate too.

---

## 4. PHASE C — LAYER 5b EXACT PLAN

Build order is **mandatory**; do not skip/reorder without a DDR. Each slice:
runs `graph_session_primer()` + `graph_deps()` before editing; writes its
ADR/DDR **before** code; adds `smoke-user` coverage; keeps `-Werror` clean;
updates `docs/build_status.md` in the same commit; and must pass its gate before
the next slice starts.

1. **DDR-5b (design, written FIRST)** — the syscall-expansion plan; the validated
   user-pointer contract (`copyin` / `copyout` / `copyinstr`, **EFAULT, never
   panic**, all user pointers validated against the process AS); and the explicit
   **fork = copy-all-pages now, COW deferred** decision.
2. **`copyin` / `copyout` primitives** + a bad-pointer / guard-page test
   (→ `EFAULT`, kernel survives). Comes after DDR-5b.
3. **`read` / `write`** (using the validated copy path).
4. **`open` / `close`.**
5. **`lseek` / `fstat` / `getcwd`.**
6. **`mmap` baseline** (MAP_ANON; W^X-respecting flags).
7. **`execve`** (replace the AS; reuse the ELF loader).
8. **`wait4`.**
9. **Signal groundwork** (+ the fork decision record if true COW is deferred).

**Gate report format (print after each slice gate, exactly):**
```
Phase 5b — gate report
<commit> | <gate> | PASS/FAIL | next: <next slice>
```

---

## 5. GRAPH TOOLING (use it FIRST, every session)

- **Location:** `tools/graph_mcp/` — pure JS/WASM code knowledge graph (MCP + CLI).
  Full usage: `tools/graph_mcp/CLAUDE_GRAPH_USAGE.md`.
- **MCP server:** registered in `/.mcp.json` (`pradyos-graph`). The client prompts
  once to trust it. Manual start: `node tools/graph_mcp/server.js mcp`.
- **Fresh clone / first run:** `bash tools/graph_mcp/setup_graph.sh`
  (ensures Node ≥18, `npm ci`, builds + validates the graph). On Windows:
  `cd tools/graph_mcp && npm ci && node server.js rebuild`.
- **MANDATORY workflow:**
  - **`graph_session_primer()` before opening ANY source file** (orientation).
  - **`graph_deps("<file>")` before editing any file** (includes / included-by / users).
  - **`graph_blast_radius("<file>")` before any refactor or signature change.**
  - `graph_query("…")` / `graph_files("<subsystem>")` to locate code instead of blind reads.
  - `graph_callchain("<fn>")` before changing a function's contract.
  - **`graph_rebuild()` after structural changes** (new/renamed/moved files, new functions).
- **CLI fallback** (if MCP not connected):
  `node tools/graph_mcp/server.js {primer|query <t>|files <ss>|deps <f>|callchain <fn>|blast <f>|rebuild|selftest}`.

---

## 6. CI DETAILS

- **Workflow:** `.github/workflows/ci.yml`. Runs on push to `main` / `dev/**` and PRs.
- **Job `build-and-boot`** — must stay green:
  `toolchain-check` → `image` (`-Werror`) → `smoke` → `smoke-fs` → `smoke-fs-rw`
  → `smoke-fs-sfs-rw` → `smoke-fs-ext4` → `smoke-user`.
- **Job `code-graph`** (isolated, non-destructive) — `npm ci` then
  `node server.js selftest`: builds the graph from source and validates the 7
  query tools (writes only the git-ignored `.graph/` DB; cannot make kernel gates flaky).
- **Local full gate set (run before committing):**
  ```
  wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/c/Users/prady/Documents/Claude/Projects/Prady4OS \
    && source "$HOME/.cargo/env" && make toolchain-check && make image && make smoke \
    && make smoke-fs && make smoke-fs-rw && make smoke-fs-sfs-rw && make smoke-fs-ext4 && make smoke-user'
  ```
- **Verify CI from the host:** `gh run list/watch --repo prady4the4bady/Prady4OS`
  (use the Windows `gh`; it is not on the WSL PATH).

---

## 7. TOOLCHAIN NOTES

- **Builds run in WSL**, not native Windows:
  `wsl -d Ubuntu-24.04 -- bash -lc 'cd <repo> && source "$HOME/.cargo/env" && make <target>'`.
  Cross toolchain (ADR-001): clang + `ld.lld` + `llvm-objcopy` + nasm; Rust nightly
  `x86_64-unknown-none`; QEMU q35.
- **WSL has no Linux `node`** — only the Windows `node`/`npm` leak in via PATH
  interop. So the graph tooling uses a **pure JS/WASM stack** (`sql.js` +
  `web-tree-sitter@0.22.6` + `tree-sitter-wasms`): one `node_modules` works on the
  Windows host, WSL, **and** Linux CI — no `node-gyp`, no native prebuilds, no CI
  flakiness. `setup_graph.sh` installs a Linux Node (≥18) in WSL when missing.
- **Shell-quoting gotcha (cost real time):** inline shell variables and loops
  inside `wsl … bash -lc '…'` from the agent's Bash tool **expand to empty**
  (MSYS2/Git-Bash argv mangling) — e.g. `L=foo; … "$L"` → empty, `for g in …; do
  make $g; done` → no target. `$HOME`, `$(…)`, and `<<'EOF'` heredocs survive. Use
  **literal commands / fixed paths / heredocs**, or put loops in a Makefile target.
- **WSL git identity** is per-repo (set this session): user.name
  `pradyun kumar sinha`, user.email `pradyun4kumar4sinha4@icloud.com`. A new
  account should set its own.
- **NASM** is parsed by a focused extractor (not Tree-sitter) — ASM grammars lack
  reliable prebuilt WASM; this is intentional and documented.

---

## 8. FILE OWNERSHIP MAP

| Path | Layer / role | Read ADR before editing |
|------|--------------|--------------------------|
| `boot/mbr/`, `boot/stage2/` | L1 bootloader | ADR-005 |
| `arch/x86_64/*.asm` | L2 asm (boot, cpu, isr, context, syscall_entry, usermode) | ADR-005/012 |
| `arch/x86_64/user_image.asm` | L5 — embeds `build/hello.elf` + `build/wxviol.elf` | ADR-021 |
| `kernel/mm/pmm.*`, `kheap.*` | L2 memory | ADR-003 |
| `kernel/mm/vmm.*` | L2 VMM + **per-process AS / NX** | **ADR-007 + ADR-021 (binding)** |
| `kernel/proc/sched.*`, `tss.*` | L2 scheduler + per-process CR3 | ADR-008 (+ ADR-021 for cr3) |
| `kernel/cap.*` | L2 capabilities | ADR-009 |
| `kernel/ipc/*` | L2 IPC + broadcast | ADR-010/011 |
| `kernel/syscall/*` | L2 syscalls (5b extends this) | ADR-012 |
| `kernel/idt.c` | L2 exception dispatch + **user-fault kill path** | ADR-021 |
| `kernel/exec/elf.*` | **L5 ELF loader (binding security)** | **ADR-021** |
| `kernel/acpi/*`, `kernel/drivers/*` | L3 drivers | ADR-013/014/020 |
| `kernel/fs/vfs/*` | L4 VFS + mounts | ADR-015/017 |
| `kernel/fs/fat32/*` | L4 FAT32 RW + LFN | ADR-015/020 |
| `kernel/fs/sfs/*` | L4 SFS engine | **ADR-018** |
| `kernel/fs/ext4/*` | L4 ext4 read-only | ADR-019 |
| `kernel/main.c` | boot orchestration + in-kernel self-tests | — |
| `user/hello.asm`, `user/wxviol.asm`, `user/user.ld` | L5 ring-3 test programs | ADR-021 |
| `tools/graph_mcp/**` | tooling (not a kernel layer) | `CLAUDE_GRAPH_USAGE.md` |
| `docs/decisions/ADR-*.md` | architecture decisions | — |
| `docs/build_status.md`, `docs/platform_profiles.md` | status (update with code) | — |

**Generated — never commit (git-ignored):** `build/`, `tools/graph_mcp/.graph/`,
`tools/graph_mcp/node_modules/`, `*.o`/`*.elf`/`*.bin`/`*.img`, `target/`.
The graph DB and `node_modules` are reproduced via `npm ci` + `rebuild`.

---

## 9. WHAT NOT TO DO

- ❌ **Do not weaken W^X or NX to make a test pass.** Fix the test or the real bug.
- ❌ **Do not commit generated artifacts** (`build/`, `.graph/`, `node_modules/`, images, objects).
- ❌ **Do not patchwork.** Root-cause fixes only; ADR/DDR before governed code.
- ❌ **Do not claim CI is green without verifying the current run** (`gh run watch`).
- ❌ **Do not proceed past a failing gate** — stop and fix the root cause first.
- ❌ **Do not skip `graph_session_primer()` at session start**, or `graph_deps()`
  before editing, or `graph_blast_radius()` before a refactor.
- ❌ **Do not start slice N+1** until slice N boots clean and passes its gate.
- ❌ **Do not invent ISA/register details** — cite Intel/AMD SDM or say "I don't know".
- ❌ **Do not write the PAT/secrets into any tracked file.**

---

*Generated at the end of the session that completed Layer 5a (ADR-021) and added
the `tools/graph_mcp` code knowledge graph. HEAD `4608e9b`. Next: Phase C, Slice 1
— DDR-5b.*
