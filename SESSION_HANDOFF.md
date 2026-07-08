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

### 0.0 CURRENT STATE (updated 2026-07-04 — supersedes everything below)

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
  `u_*` block (static asserts enforce). **Still open: per-wake
  `smp_resched_one` IPIs** — idle APs pick up new work on their own 100 Hz tick.
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
  decorations, alt-tab all shipped. Remaining L7 (non-visual): surface destroy;
  per-agent live metrics; SFS `/etc/aether/config`; `CAP_NET` gate;
  wlroots/Wayland (the standing out-of-tree wall).
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
