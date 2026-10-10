# PRADYOS — OS component checklist

Status of every component, read from the tree on `dev/phase1` (2026-10-10) and
from the gate list in `tools/ci/gate_shards.txt`. One row per component.

Legend: **DONE** = built and CI-gated · **PARTIAL** = built, named parts missing ·
**DEFERRED** = recorded deferral or refusal · **ABSENT** = no code in the tree ·
**OPEN** = known defect still open.

"Gated" means a `smoke-*` target exists and is registered in a CI shard. It does
not mean the component is free of defects (see OPEN rows).

## Boot

| Component | Status | Evidence / gate | Notes |
|---|---|---|---|
| BIOS boot (stage1/stage2) | DONE | `smoke`, `smoke-iso-x86` (BIOS arm) | 1.5 MiB load window (§INV.18) |
| UEFI loader | DONE | `smoke-uefi`, `smoke-iso-x86` (UEFI arm) | |
| UEFI GOP framebuffer | DONE | `smoke-gop` (DDR-1142) | |
| x86_64 ISO | DONE | `smoke-iso-x86`, `smoke-iso-userspace` | BIOS + UEFI from one ISO |
| aarch64 / riscv64 | PARTIAL | `smoke-aarch64`, `smoke-riscv64` (own CI job) | Boot stub only (ADR-034). No MMU, no userspace, no ISO |
| Installer (to disk) | DONE | `smoke-install` (DDR-1143) | BIOS + UEFI installed boot |
| Secure boot / TPM / measured boot | DEFERRED | DDR-1144/1145/1146 designs | Awaiting operator decisions |
| KASLR | DEFERRED | DDR-1051 | Sequencing call, not a blocker |

## Kernel core

| Component | Status | Evidence / gate | Notes |
|---|---|---|---|
| IDT / exceptions / panic path | DONE | `smoke-mce`, panic report in CI logs (DDR-1088) | |
| #MC machine check | DONE | `smoke-mce` | No recovery; every #MC fatal |
| PMM / buddy / NUMA nodes | DONE | `smoke-pmm-poison`, `smoke-numa`, `smoke-numa-alloc` | |
| VMM / paging / COW | DONE | `smoke-cowfork`, `smoke-sharedpte` | |
| Kernel heap (kheap) | DONE | KHEAP_DEBUG always on | OPEN-13 (one double-free capture) |
| Kernel W^X + CR0.WP | DONE | `smoke-wxkernel` (DDR-1046, DDR-1126) | |
| SMEP / SMAP | DONE | `smoke-smep`, `smoke-smap` | AC-across-interrupt gap recorded |
| Spectre/Meltdown exposure line | DONE | `smoke-mitigations`, `smoke-cpuexposure` | KPTI/retpoline deferred (DDR-1140) |
| Demand-paged user stack | DONE | `smoke-stack-demand` | |
| User-copy (copyin/copyout) | DONE | `smoke-uaccess` | |
| Scheduler + per-CPU runqueues | DONE | `smoke-smpsched`, `smoke-rqstress`, `smoke-percpu-sched` | OPEN-2 (see below) |
| Timed block / wakeups | DONE | `smoke-blk-timeout`, `smoke-crosswake` | |
| SMP bring-up / IPIs | DONE | `smoke-smp`, `smoke-resched`, `smoke-smpuser` | |
| LAPIC timer / I/O APIC | PARTIAL | `smoke-apic` | 8259 cutover (stage D) deferred, DDR-1050 |
| MSI-X | DONE | `smoke-msixap` | |
| ACPI poweroff / reboot | DONE | `smoke-poweroff`, `smoke-reboot` | |
| Spinlock contention instrument | DONE | no gate (by design, DDR-1047) | |
| Signals | PARTIAL | `smoke-syssignal`, `smoke-kill`, `smoke-killblock` | No sigprocmask, SA_SIGINFO, SIGCHLD |
| Checkpoint / resume | DONE | `smoke-checkpoint` | Fix 2 (DDR-1157) on PR #29 |
| Cross-CPU TLB shootdown | ABSENT | tripwire `ci-cr3-writers-check` | Correct today; prerequisite of threads |
| pthread / CLONE_VM | ABSENT | — | Needs shootdown first |
| Futex | ABSENT | DDR-1038 | Needs shared memory |

## Drivers

| Component | Status | Evidence / gate | Notes |
|---|---|---|---|
| PCIe enumeration | DONE | `smoke-chipset` | |
| virtio-blk | DONE | `smoke-blkmq`, `smoke-blk-integrity` | Fix 1 (DDR-1156, PR #29), Fix 4 (DDR-1160, PR #35) |
| AHCI | DONE | `smoke-ahci` | |
| NVMe | PARTIAL | `smoke-nvme` | Polled; completion IRQ deferred. F3 (PRP pages on timeout) recorded, not approved |
| Partitions / MBR | DONE | `smoke-part` | |
| virtio-net + e1000e | DONE | `smoke-net`, `smoke-e1000e` | |
| virtio-gpu / framebuffer | DONE | `smoke-gpu`, `smoke-fb` | |
| PS/2 keyboard + modifiers | DONE | `smoke-modkeys`, `smoke-input` | |
| virtio-input (tablet) | DONE | `smoke-mouse` | |
| RTC | DONE | `smoke-time`, `smoke-rtc-smp` | |
| virtio-rng | DONE | `smoke-rng` | |
| fw_cfg | DONE | used by every probe gate | |
| **USB (xHCI/EHCI, HID, mass storage)** | **ABSENT** | — | `grep -ril usb kernel/` returns nothing. No host controller, no class drivers, no gate |
| Audio (Intel HDA) | DEFERRED | pre-approved exception | |

## Filesystems / storage

| Component | Status | Evidence / gate | Notes |
|---|---|---|---|
| VFS | DONE | `smoke-fs`, `smoke-rename`, `smoke-fsrm` | |
| FAT32 | DONE | `smoke-fs-rw`, `smoke-fat32-multicluster` | |
| SFS (B+tree, journal, LZ4) | DONE | `smoke-sfs-btree`, `smoke-sfs-gc`, `smoke-sfs-persist` | 4-extent file limit; on-disk free tree deferred |
| ext4 | PARTIAL | `smoke-fs-ext4` | Read-only |
| Block flush / barriers | DONE | `smoke-part` (flush arms) | |
| Quotas, >512 mkfs slots | ABSENT | — | |

## Networking

| Component | Status | Evidence / gate | Notes |
|---|---|---|---|
| lwIP TCP/UDP (kernel) | DONE | `smoke-net-lo`, `smoke-net-tcp-lo`, `smoke-nethammer` | |
| DHCP + DNS | DONE | `smoke-dhcpdns` | |
| Ring-3 TCP proxy sockets | DONE | `smoke-capnet`, `smoke-netallow` | |
| CAP_NET allowlist / privacy mode / egress audit | DONE | `smoke-privacy-netfilter`, `smoke-egress-audit` | |
| Ring-3 UDP | ABSENT | — | Needs per-send enforcement (DDR-1091) |
| epoll on sockets | ABSENT | DDR-1069 | Sockets are not fds |
| IPv6 | DEFERRED | `LWIP_IPV6 0` | |
| TLS | ABSENT | DDR-1104 | Primitives in tree; trust anchor is the blocker |

## Userspace

| Component | Status | Evidence / gate | Notes |
|---|---|---|---|
| ELF loader, execve argv/envp | DONE | `smoke-sysexec`, `smoke-execve-argv` | |
| fork / wait / pipes / epoll / poll / io_uring | DONE | `smoke-sysfork`, `smoke-syswait`, `smoke-syspipe`, `smoke-sysepoll`, `smoke-poll`, `smoke-sysiouring` | io_uring indices inert (DDR-1108) |
| mmap / mprotect (incl. file MAP_PRIVATE) | DONE | `smoke-sysmmap`, `smoke-mprotect` | No MAP_SHARED |
| musl libc | DONE | `smoke-user` | Static only; no dynamic linker |
| init + service manager | DONE | `smoke-init` | |
| PRISM shell (pipes, redirect, quoting, jobs, wait, source) | DONE | `smoke-shell` | `bg` refused (no job-control signals) |
| Terminal window (Ctrl+Alt+T) | DONE | `smoke-ctrlaltt` | |
| Package manager `prad` | ABSENT | — | NSI 88–90 reserved |

## Desktop / compositor

| Component | Status | Evidence / gate | Notes |
|---|---|---|---|
| Compositor, windows, focus, z-order | DONE | `smoke-compositor`, `smoke-focus`, `smoke-wmorder` | |
| Move / resize all edges / maximize / minimize / restore | DONE | `smoke-drag`, `smoke-resizeall`, `smoke-wmmax`, `smoke-wmmin`, `smoke-perrestore` | |
| Alt+Tab, Super+M | DONE | `smoke-alttab`, `smoke-superkey` | |
| Ambiance / theme / horizon bands | PARTIAL | `smoke-ambiance`, `smoke-horizon` | Animated mesh not built |
| Surface lifecycle / close events | DONE | `smoke-surfclose`, `smoke-surfdestroy` | OPEN-1 route 1 stated open |

## AETHER / agents / security

| Component | Status | Evidence / gate | Notes |
|---|---|---|---|
| Action queue + policy + DAG | DONE | `smoke-aether`, `smoke-actiondag` | |
| Section 3C action types (8 of 8) | DONE | `smoke-actionread/del/query/hypo`, `smoke-runexp`, `smoke-sendipc`, `smoke-coderewrite` | |
| Agent roster / metrics / kill | DONE | `smoke-agents`, `smoke-agentmetrics`, `smoke-killagent` | Fix 3 (DDR-1159, PR #34) |
| IPC door | DONE | `smoke-sendipc` | |
| Experiment machine | DONE | `smoke-runexp` | |
| Audit chain (tamper-evident) | DONE | `smoke-auditchain`, `smoke-auditchain-tamper` | Durable flush not built |
| Signed ledger (ML-DSA, per-install key) | DONE | `smoke-ledger`, `smoke-install` | |
| Domain capabilities (OCR/SCENE/BROWSE) | PARTIAL | `smoke-domcap` | Enforced at submission; no executor |
| Domain agents F#66–F#75 | ABSENT | — | |
| Crypto (SHA-2/3, HKDF, AEAD, X25519, Ed25519, ML-DSA-44) | DONE | `smoke-sha256`, `smoke-sha512`, `smoke-shake`, `smoke-hkdf`, `smoke-aead`, `smoke-x25519`, `smoke-ed25519`, `smoke-mldsa` | |
| Vault / lockbox / AGS / ACC | DONE | `smoke-vault`, `smoke-lockbox`, `smoke-ags`, `smoke-acc` | |
| Invariant suite S1–S8 | PARTIAL | `smoke-invariants` | S3/S7 depend on domain agents |

## Open defects (do not close by checklist)

| Issue | Status |
|---|---|
| OPEN-1 route 1 (CI-only hang) | OPEN, stated open (DDR-1124) |
| OPEN-2 (AP freeze family) | Double-dispatch mechanism fixed (DDR-1139); other producers unattributed |
| OPEN-12, OPEN-13 | OPEN, one capture each |
| U1, U2, U3 (DDR-1155) | OPEN; Fixes 1–4 do not close them |
| POSIX `sys_kill` unlocked walk | Recorded in DDR-1159 §7, proposed, not fixed |
| NVMe PRP pages on timeout (F3) | Recorded, not approved |
