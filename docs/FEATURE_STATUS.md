# PRADYOS — Feature status (measured 2026-10-14, DDR-1166)

**How this was produced.** Every status below was checked against the tree at
`repo-audit-ddr1166` (stacked on the USB piece-5 tip `fdb5097`). A feature is
**gated** only if a `smoke-*` target with that name exists in the `Makefile`
AND is assigned in `tools/ci/gate_shards.txt`. `make ci-shard-check` reads
**188 gates, 10 shards, 6 excluded**. `kernel.bin` = `d8d48f39c25e0ff9`, 1,483,146 B,
warning-clean at `-Werror` after a clean rebuild.

**Added 2026-11-12 (DDR-1201, DDR-1200; CodeRabbit on #123): a second way to be gated.** A gate that rebuilds the kernel with a test flag cannot run in a shard, because every shard asserts that `kernel.bin` did not change while its gates ran. Such a gate counts as **gated** when its `smoke-*` target exists in the `Makefile`, it is in `.github/workflows/ci.yml`'s `flag-gates` matrix, and `tools/ci/shard_check.sh` excludes it from the shards with that reason. Three gates use this path: `smoke-zanchor`, `smoke-rcu` and `smoke-lockdep` (DDR-1209).

**Re-measured 2026-10-15 on the combined tree `d201c2c`** (after #43, #44, #46,
#47 merged without a combined-tree run): `make ci-shard-check` reads **193 gates,
10 shards, 6 excluded**; `kernel.bin` = `7d429743e211a591`, **1,503,626 B**
(69,238 B headroom), warning-clean at `-Werror`; `tools/ci/hygiene_check.sh`
ALL TEN PASSED; `smoke-shell` rc=0 locally. Statuses below are unchanged except
where marked.

Status words:

* **gated**: built, and asserted by a CI-registered gate.
* **partial**: part of the stated scope is built and gated; the rest is listed.
* **ungated**: built, with no gate asserting it.
* **not started**: no code.
* **deferred**: recorded operator deferral; do not pull forward.

Gates that exist only on `dev/phase1-seyp3n` (PR #29, not yet in
`dev/phase1`) are marked `(seyp3n)`.

## Boot, CPU, memory

| Feature | Status | Gate(s) | Use case |
|---|---|---|---|
| BIOS MBR + stage2 boot | gated | `smoke` (every gate boots this) | Boot on legacy firmware and in QEMU |
| UEFI loader | gated | `smoke-uefi` | Boot on modern firmware |
| UEFI GOP framebuffer | gated | `smoke-gop` | Display on UEFI machines without a GPU driver |
| Hybrid ISO (BIOS + UEFI) | gated | `smoke-iso-x86`, `smoke-iso-userspace` | Bootable release media; live OS on SFS root |
| Install to disk | gated | `smoke-install` | Install from the live ISO and boot the installed disk |
| Buddy PMM, poison | gated | `smoke-pmm-poison` | Physical memory; catches use-after-free |
| NUMA topology / alloc / steal | gated | `smoke-numa`, `smoke-numa-alloc`, `smoke-numa-steal` | Node-local memory and scheduling |
| COW fork, shared-PTE accounting | gated | `smoke-cowfork`, `smoke-sharedpte` | Cheap fork |
| Demand-paged user stack | gated | `smoke-stack-demand` | Stacks grow on use; guard page kills overflow |
| Kernel W^X + CR0.WP | gated | `smoke-wxkernel` | Kernel text is not writable, even from ring 0 |
| SMEP / SMAP | gated | `smoke-smep`, `smoke-smap` | Kernel cannot execute or read user pages by accident |
| Spectre/Meltdown mitigations, exposure line | gated | `smoke-mitigations`, `smoke-cpuexposure` | States whether this CPU is exposed |
| Machine check (#MC) | gated | `smoke-mce` | A hardware fault reports instead of triple-faulting |
| KASAN UAF regression | gated (seyp3n) | `smoke-kasan-uaf` | Freed memory read is caught |
| CPU model matrix | partial | `smoke-chipset` (4 QEMU models) | Runtime CPUID dispatch exists for ERMS (memcpy/memset) and NX/SMEP/SMAP/MCE; no per-microarchitecture code paths beyond that. Log path fixed in DDR-1166 §3. |
| KASLR | deferred (DDR-1051) | none | — |

## Scheduler, SMP, IPC

| Feature | Status | Gate(s) | Use case |
|---|---|---|---|
| SMP bring-up, per-CPU runqueues, stealing, resched IPI | gated | `smoke-smp`, `smoke-smpsched`, `smoke-smppreempt`, `smoke-percpu-sched`, `smoke-resched`, `smoke-rqstress`, `smoke-rqstress-liveness`, `smoke-crosswake` | Multicore scheduling |
| TCB freed-while-queued fix | gated | `smoke-rqfree` | Reaper cannot free a queued thread |
| Tick sweep ends when the current thread is off the ring (#115, DDR-1201) | gated in a test build with the window held open (emulated-verified; not verified on real hardware) | `smoke-zanchor` A, B | A timer tick cannot loop forever on a thread that is exiting |
| Kernel RCU grace periods: IF-off read sections, tick-counter grace periods; first user, the timer-tick timed-wake sweep (DDR-1200, C1.4 P5a and P5b) | gated in a test build (emulated-verified; not verified on real hardware) | `smoke-rcu` RA, RB, RC, RD, RE, RF | Kernel code can read shared structures without a lock and free them only when no reader can still see them. The timer tick now times out waits without taking the scheduler lock, and a thread's memory is freed only after a grace period |
| Clocks and sleep: `CLOCK_MONOTONIC` and `CLOCK_REALTIME` on the TSC through the vDSO, `SYS_CLOCK_GETTIME` (120), `SYS_NANOSLEEP` (121) (C1.5 P1, DDR-1188) | gated (emulated-verified; not verified on real hardware) | `smoke-schedclass` A, B, B2, B3 | Time that never goes back; a sleep that never ends early and can be interrupted |
| One-shot LAPIC timers: a sleep can end between ticks (C1.5 P2, DDR-1188 A3) | gated (emulated-verified; not verified on real hardware) | `smoke-schedclass` C, M | Sub-tick wake-ups, with a per-thread budget |
| `SCHED_FIFO` and `SCHED_RR`, the 19-of-20-tick RT throttle, `RES_SCHED` refusals, one-level `kmutex` priority inheritance (C1.5 P3, DDR-1188 A4). Not gated: a successful `RES_SCHED` grant and its spread to threads (Amendment 4) | gated (emulated-verified; not verified on real hardware) | `smoke-schedclass` D, E, F, G, H, HA, N, P | Real-time threads ahead of the fair class, without starving it |
| `SCHED_DEADLINE`: EDF with a constant-bandwidth server, admission at 0.90 per CPU, pinned threads (C1.5 P4, DDR-1188 A5) | gated (emulated-verified; not verified on real hardware) | `smoke-schedclass` I, J, Q | Periodic work with a guaranteed budget |
| Pressure stall information `some`, per CPU and for virtio-blk reads, `SYS_PSI` (124) (C1.5 P5, DDR-1188 A6). Memory pressure not built (C2.1) | gated (emulated-verified; not verified on real hardware) | `smoke-schedclass` K, K0, O | Tell whether a CPU or the disk is the bottleneck |
| Tickless idle on the APs: an idle AP stops its tick, a sleeper on it still wakes at its deadline, and RCU treats it as quiescent; the BSP never stops (C1.5 P6, DDR-1188 A7). Not gated: the 1 s bound as a backstop and the "CPU frequency" CPUID report (Amendment 7) | gated (emulated-verified; not verified on real hardware) | `smoke-schedclass` L, L1, L2 | An idle AP takes no timer interrupts; a sleeper on it still wakes between ticks |
| Lock-order checking for kernel spinlocks: deadlock detection (DDR-1209, C1.4, #152). The shipped kernel checks nothing; the checking build is `LOCKDEP=1`. The checker freezes at about 240 s, and locks taken after that are not checked | gated in a test build (emulated-verified; not verified on real hardware) | `smoke-lockdep` LA, LB, LC, LD, LE | A lock-order inversion is reported the first time the second order runs, whether the two paths ever met or not. Two locks of one annotated class taken in opposite orders are counted as same-class nesting (D5). The first such event per class prints a `[lockdep] nest` report, and none is reported as an inversion. The gate also runs the thread probe under the checker and requires no inversion in the real kernel |
| `mprotect` cannot add write to a frame another process maps (#112, DDR-1202) | gated (emulated-verified; not verified on real hardware) | `smoke-mprotect` F, G, H | One process cannot change another's private memory, or the shared vDSO page |
| OPEN-2 double dispatch (DDR-1139) | gated (fix confirmed) | `[schedcheck]` in GLOBAL_FORBIDDEN | One thread on one CPU at a time |
| Capability IPC, ring-3 door | gated | `smoke-sendipc` | Agent-to-agent messages |
| Signals (4), SIGKILL from blocked waits | gated | `smoke-syssignal`, `smoke-kill`, `smoke-killblock`, `smoke-sigpipe` | Stop a runaway process |
| POSIX signal semantics: mask, SA_SIGINFO, SIGCHLD, EINTR, SA_RESTART, syscall-exit delivery | gated (emulated-verified; not verified on real hardware) | `smoke-sigsem` | Programs can block, inspect and safely restart around signals |
| `sigprocmask`, `SA_SIGINFO`, `SIGCHLD`, `SA_RESTART` | not started | none | DDR-1090: no EINTR, no mask |
| `CLONE_VM` threads, TLS, thread groups (DDR-1181) | gated (emulated-verified; not verified on real hardware) | `smoke-thread` A, B, C, F | Several threads of one program share memory; #76 `3358b35` |
| `pthr.h`, a native pthreads subset on `psync.h` (DDR-1197) | gated (emulated-verified; not verified on real hardware) | `smoke-thread` PA, PJ, PR, PD, PG, PK, PO | Create, join, detach and reap, guard page, keys, once; freestanding programs only; #111 `2a508d5`. Not gated: a fork during create, a self-detach |
| Cross-CPU TLB coherence for threads, CPU pinning (DDR-1177, DDR-1186) | gated, QEMU only (not verified on real hardware) | `smoke-thread` P, D, E, M, G | `mprotect`/`munmap` on one CPU is seen by a thread on another; #91 `f0e1bb6` |
| User mutex, semaphore, condition variable, barrier on futex (`psync.h`, DDR-1193) | gated (emulated-verified; not verified on real hardware) | `smoke-thread` SA, SW, SB, SC, SH, SD | Threads in one program can lock, signal and meet at barriers; #101 `9306963`. SA catches a non-atomic acquire only probabilistically; no timed waits yet |
| futex: WAIT/WAKE, private and shared keys, timeout, signal wake, CLEARTID wake (DDR-1181 A1) | gated (emulated-verified; not verified on real hardware) | `smoke-thread` FA-FJ | Threads can sleep on a memory word instead of spinning; #81 `cc99745`. Limit: a default-terminate signal other than SIGKILL, with no handler, does not end a FUTEX_WAIT (DDR-1181 A1.3) |

## Syscalls, files, storage

| Feature | Status | Gate(s) | Use case |
|---|---|---|---|
| POSIX core (read/write/open/fork/exec/wait/mmap/pipe/epoll/poll/mprotect/ftruncate/rename/unlink) | gated | `smoke-sys*`, `smoke-poll`, `smoke-mprotect`, `smoke-ftruncate`, `smoke-rename*`, `smoke-execve-argv` | User programs |
| File-backed `MAP_PRIVATE` mmap | gated | `smoke-sysmmap` | Map a file read-only/private |
| io_uring (read/write, aligned ring) | partial | `smoke-sysiouring` | Batched I/O; no FSYNC/OPENAT/eventfd/chaining, index fields inert |
| FAT32 r/w | gated | `smoke-fs`, `smoke-fs-rw`, `smoke-fat32-multicluster` | Boot volume, user binaries |
| SFS (B+tree, dirs, unlink, GC, persistence, LZ4, journal) | gated | `smoke-sfs-*`, `smoke-fs-sfs-rw`, `smoke-mkfs-sfs` | Native filesystem and root |
| SFS large files (>16 KiB from ring 3) | not started | none | DDR-1100: blocked on staging chunk and `inline_extents[4]` |
| ext4 read / write | partial | `smoke-fs-ext4` (read) | Read Linux disks; write not started |
| virtio-blk multi-queue, MSI-X, integrity | gated | `smoke-blkmq`, `smoke-blk-integrity`, `smoke-msixap` | Disk I/O |
| virtio-blk: a timed-out request keeps its memory with the device (per-slot bounce buffers; a timed-out slot is held until its late completion) (DDR-1208) | gated (emulated-verified; not verified on real hardware) | `smoke-vblklate` Q, B | A caller may free its buffer after `-EIO`; a late completion cannot write into reused memory |
| AHCI, NVMe (poll mode) | gated | `smoke-ahci`, `smoke-nvme` | Real SATA / NVMe disks; NVMe completion IRQ deferred |
| Partitions (MBR) | gated | `smoke-part` | Installed-disk layout |

## Devices

| Feature | Status | Gate(s) | Use case |
|---|---|---|---|
| virtio-net + lwIP, TCP/UDP loopback, DHCP + DNS | gated | `smoke-net*`, `smoke-dhcpdns` | Networking |
| e1000e | gated | `smoke-e1000e` | Intel NIC |
| PS/2 keyboard + modifiers, virtio-input mouse | gated | `smoke-input`, `smoke-modkeys`, `smoke-mouse` | Keyboard and pointer |
| USB xHCI: controller, slot, address, config, continuous HID boot keyboard | gated | `smoke-xhci`, `smoke-usbkbd` | A USB keyboard types into the same rings as PS/2 (NSI 46/96), polled from the BSP tick (DDR-1167) |
| USB hubs, mouse, mass storage | not started | none | T1 register |
| RNG (virtio-rng) | gated | `smoke-rng` | Key material |
| ACPI poweroff / reboot | gated | `smoke-poweroff`, `smoke-reboot` | Shutdown |
| Audio (HDA) | deferred | none | — |
| IPv6, TLS, TAP | not started | none | DDR-1104 |

## Userspace

| Feature | Status | Gate(s) | Use case |
|---|---|---|---|
| musl libc programs | gated | `smoke-user`, `smoke-smpuser` | Ported C programs |
| init + service manager | gated | `smoke-init` | Start/supervise services |
| PRISM shell (pipes, redirection, quoting, erase, jobs, `wait`, `source`, `audit`) | gated | `smoke-shell` | Interactive shell |
| User-space ASLR: stack top, mmap base and `ET_DYN` image base drawn per exec (DDR-1189 A1 and A2, C1.6 P1 #154, P2 #156) | gated (emulated-verified; not verified on real hardware) | `smoke-dynlink` A, B, C2 | 16 bits for the stack top and 18 each for the mmap base and the PIE base (DDR-1189 L3), fewer than Linux. `ET_EXEC` images stay at their link addresses. The gate runs with virtio-rng; without a hardware source the TSC path runs, which is weak and not called secure |
| Static PIE: `ET_DYN` without `PT_INTERP`, `R_X86_64_RELATIVE` applied by the loader after a writable-target check, `AT_PHDR`/`AT_ENTRY` for `ET_DYN` (DDR-1189 A2, C1.6 P2, #156) | gated (emulated-verified; not verified on real hardware) | `smoke-dynlink` C1–C5 | `PT_GNU_RELRO` is not enforced; a musl `rcrt1` static PIE is untested; the entry check, the shared-page rule and the other P2-2 refusals are not gated |
| Kernel half of dynamic linking: `PT_INTERP` loaded by the kernel (`AT_BASE`, no kernel relocation), L1 W^X across time in `mmap`/`mprotect`, `MAP_FIXED` over the caller's own private areas (DDR-1189 A3, C1.6 P3a, #158) | gated (emulated-verified; not verified on real hardware) | `smoke-dynlink` F0–F6, M0–M7, D0a–D0c, D0r | Only a freestanding test interpreter (`/LIB/LDTEST.SO`) is exercised; musl `ld.so` is P3b. Per-area `ever_w` and a replacement never setting `x_ok` refuse safe requests (follow-up arms F7, M8). An interpreter path that is relative or unterminated, a non-`ET_DYN` interpreter, a second `PT_INTERP` and an `ET_EXEC` ending above `USER_PIE_TOP` are refused but not gated |
| Shared libraries and shared text: musl `ld.so`/`libc.so` (C1.6 P3b), shared text pages (P4) | not started (P4 needs C2.1) | none | — |

## Desktop (compositor)

| Feature | Status | Gate(s) | Use case |
|---|---|---|---|
| Framebuffer compositor, surfaces, focus, z-order | gated | `smoke-compositor`, `smoke-surface`, `smoke-focus`, `smoke-wmorder` | Windows |
| Close / maximize / minimize / per-window restore | gated | `smoke-wmclose`, `smoke-wmmax`, `smoke-wmmin`, `smoke-perrestore`, `smoke-surfclose` | Window management |
| Resize from any edge, drag | gated | `smoke-resizeall`, `smoke-drag` | Window management |
| Sovereign / Manual mode toggle (Super+M, kernel mode) | gated | `smoke-mode`, `smoke-superkey` | Switch who drives the machine |
| Four ambiances + Regalia/Consort palettes | gated | `smoke-ambiance` | Visual theme |
| Horizon bands | partial | `smoke-horizon` | Animated mesh not built |
| Alt+Tab, Ctrl+Alt+T terminal window | gated | `smoke-alttab`, `smoke-ctrlaltt` | Switching, terminal |
| Live agent panel (4 pips) | partial | `smoke-agentpanel`, `smoke-agentmetrics` | CPU% sparkline, memory graph, action histogram not built |
| Taskbar magnify | not started | none | — |

## AETHER agent layer (kernel)

| Feature | Status | Gate(s) | Use case |
|---|---|---|---|
| Action queue, policy, DAG, forced-pending types | gated | `smoke-aether-queue`, `smoke-actiondag`, `smoke-actiondel` | Agent proposes, operator approves |
| All 8 Section 3C action types end to end | gated | `smoke-action*`, `smoke-coderewrite`, `smoke-runexp`, `smoke-sendipc` | Agent actions with audit |
| Agent executes its approved write (reads it back) | gated | `smoke-aether` | Agent does the work it was approved for |
| Audit chain, tamper detection, cursor drain | gated | `smoke-auditchain`, `smoke-auditchain-tamper`, `smoke-shell` (audit) | Tamper-evident record |
| Signed audit head (ML-DSA-44, per-install key) | gated | `smoke-ledger`, `smoke-install` | Off-box verification |
| Durable on-disk audit log | not started | none | Blocked on the SFS large-file ceiling |
| Signed goals (AGS, Ed25519) | gated | `smoke-ags` | Goal integrity |
| Capabilities incl. CAP_OCR/SCENE/NET_BROWSE at submission | gated | `smoke-domcap`, `smoke-invariants` | Least privilege |
| Egress allowlist, privacy mode (connect + I/O), egress audit | gated | `smoke-netallow`, `smoke-capnet`, `smoke-privacy-netfilter`, `smoke-egress-audit`, `smoke-sovereign-egress` | Agents reach only approved hosts |
| Kill switch (SYS_KILL_AGENT, SIGKILL from blocked waits) | gated | `smoke-kill`, `smoke-killblock` | Stop a runaway agent |
| Per-agent rate limit (60 syscalls/100 ticks) | gated (as a side arm) | `smoke-aether-sec` | Cheap budget |
| Per-task budgets (time/token/byte) | gated (time, bytes, actions; tokens are not kernel-observable and are not claimed) — DDR-1170 | `smoke-taskbudget` | Block 3 |
| UI-state reading for agents | not started | none | Block 3 |
| Input injection by agents | not started | none | Block 3 |
| Sandboxed shell / browser for agents | not started | none | Block 3; browser is cloud-bridge adjacent |
| Rollback on rejection | gated (filesystem creates only; network/IPC not undone) | smoke-rollback | DDR-1171 |
| Adversarial gates (prompt injection, self-approval, budget, egress) | partial | `smoke-invariants` (S1,S2,S4,S5,S6,S8), egress gates | Self-approval refused by construction (`aether_decide` is operator-only); prompt-injection and budget-overrun gates not built |
| Local inference (Ollama on host via 10.0.2.2) | ungated in CI | `smoke-agent-live` (excluded: needs a live host) | Agents call a local model; CI runs test mode |
| Cloud inference | deferred (DDR-793) | none | Operator decision |
| Domain agents F#66–F#75 | not started | none | One agent program exists (`user/agent_base.c`) |

## Host-side tooling (not part of the OS image)

| Feature | Status | Gate | Use case |
|---|---|---|---|
| `aether/` Python host layer (build provenance, audit JSONL, invariants) | tested | `pytest aether/tests` in CI | Dev-machine tooling. It is **not** a duplicate of `kernel/aether/`; see DDR-1166 §2 |
| Code graph MCP | tested | CI selftest | Navigation |
| VirtualBox runner | ungated | `tools/vbox_runner/run_vbox.sh` (exit 77 = skipped) | Manual hardware-like check |

## Deferred (operator list; do not pull forward)

aarch64 / riscv64 beyond boot, Apple Silicon, quantum, Rust rewrites, cloud
bridge activation, CMake/Makefile hybrid, SFS on-disk free tree, NVMe completion
IRQ, KASLR.
