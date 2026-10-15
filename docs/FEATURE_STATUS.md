# PRADYOS — Feature status (measured 2026-10-14, DDR-1166)

**How this was produced.** Every status below was checked against the tree at
`repo-audit-ddr1166` (stacked on the USB piece-5 tip `fdb5097`). A feature is
**gated** only if a `smoke-*` target with that name exists in the `Makefile`
AND is assigned in `tools/ci/gate_shards.txt`. `make ci-shard-check` reads
**188 gates, 10 shards, 6 excluded**. `kernel.bin` = `d8d48f39c25e0ff9`, 1,483,146 B,
warning-clean at `-Werror` after a clean rebuild.

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
| OPEN-2 double dispatch (DDR-1139) | gated (fix confirmed) | `[schedcheck]` in GLOBAL_FORBIDDEN | One thread on one CPU at a time |
| Capability IPC, ring-3 door | gated | `smoke-sendipc` | Agent-to-agent messages |
| Signals (4), SIGKILL from blocked waits | gated | `smoke-syssignal`, `smoke-kill`, `smoke-killblock`, `smoke-sigpipe` | Stop a runaway process |
| `sigprocmask`, `SA_SIGINFO`, `SIGCHLD`, `SA_RESTART` | not started | none | DDR-1090: no EINTR, no mask |
| pthreads / `CLONE_VM` | not started | none | Blocked on a cross-CPU TLB shootdown that does not exist (DDR-1075, DDR-1077 tripwire) |
| futex | not started | none | Blocked on shared memory (DDR-1038) |

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
| Dynamic linking | not started | none | — |

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
| Rollback on rejection | not started | none | Block 3 |
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
