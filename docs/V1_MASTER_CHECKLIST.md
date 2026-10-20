# PRADYOS v1 master checklist

Operator-approved scope (issue #45, comments 6201120568, 6201467383, 6201619419):
**v1 contains the whole OS.** Claude works this file top to bottom.

## How to use this file (Claude: read first)

1. Find the first chunk whose status is not `DONE` and whose `Needs` are all `DONE`.
2. Build it. Every item needs a CI-registered gate (`smoke-*`, registered in `tools/ci/gate_shards.txt`) and a mutation check. No gate means not done.
3. Root-cause fixes only. No patchwork. Keep the repo clean and organized.
4. When the chunk's exit gate is green and merged: tick its boxes, set status to `DONE`, record the merge SHA, cut a release-candidate ISO, run the VirtualBox gate on it, post a short status comment on issue #45, update `SESSION_HANDOFF.md`.
5. Return to this file and take the next chunk. Do not stop between chunks. Do not ask the operator.
6. If a chunk is blocked: post one blocker comment on issue #45, mark it `BLOCKED(reason)`, continue with the next unblocked chunk.
7. Structure may change if a DDR records why. Update this file in the same PR.
8. Honesty labels in every status line: `gated`, `emulated-verified`, `not verified on real hardware`. Never label anything ASI. Quantum: interface plus simulator only, no speedup claims.
9. Operator order rule: C11.1 (Rust and build system) is ALWAYS the last chunk built. Do not pull it forward.

Status words: `TODO`, `IN-PROGRESS`, `BLOCKED(reason)`, `DONE`. The baseline for what already exists is `docs/FEATURE_STATUS.md`; update it as chunks complete.

Names marked `(name TBD)` for gates are placeholders; Claude chooses the real name.

---

## Phase 0. Housekeeping

### C0.1 Baseline and Dependabot
Status: IN PROGRESS. Needs: none.
- [x] Full gate run on the current `dev/phase1` tip; fix anything red at the root (run 39105021641, green)
- [x] Dependabot (DDR-1172): #19 closed (superseded by #48); merged #9 `3c1708d`, #7 `55b125d`, #8 `02ede03`, #30 `bca2795`, #33 `52b35f3`; #42 closed red (web-tree-sitter 0.25+ has no default export)
- [x] Open docs PRs #27, #31, #36: read, merge or close, record why (DDR-1173)
- [x] DDR-1171 rollback implementation on `rollback-ddr1171`: merged via #51 as `db5d701` (head `3795348` green, run 38989261040). Open owner question: the `sys_open` access-mode gap recorded in DDR-1171 is not fixed.
Exit gate: all existing gates green on one SHA; open-PR list clean.

---

## Phase 1. Kernel foundations

### C1.1 Signals and EINTR
Status: DONE. PR #52 merged `71d482e`; mutants M1–M4 shown red post-merge, gate fix and evidence in PR #53 merged `6edd623` (DDR-1174 §6). Needs: C0.1. Ref: DDR-1090, DDR-1174.
- [x] `sigprocmask`, `SA_SIGINFO`, `SIGCHLD`, `SA_RESTART`, EINTR semantics
Exit gate: `smoke-sigsem` (shard 6, strict).

### C1.2 Cross-CPU TLB shootdown
Status: DONE. Needs: C0.1. Ref: DDR-1075, DDR-1077, DDR-1177.
- [x] IPI-based TLB shootdown on unmap/mprotect: #57 `e6c2e16` (DDR), #58 `5e4ccf5` (code and gate). Gate green on 912ff68 (run 39141624725); M2 red (run 39141621302), M3 red (run 39141625093). mprotect is covered by C1.4 arm D. Emulated (TCG) only, not verified on real hardware.
Exit gate: `smoke-tlbshoot` (in-kernel probe, arms A-C, 4 CPUs; DDR-1177 Amendment 2). M2 and M3 shown red in CI before merge.

### C1.3 Shared memory
Status: IN PROGRESS. Needs: C1.2. Ref: DDR-1038, DDR-1178.
- [x] P1 anonymous `MAP_SHARED` across fork: #63 merged as `a2ac698` (the DDR from #62 rides in the same merge). Gate `smoke-shm` (shard 0), arms A-F, green on `3c6865a` (run 39425747997). Mutants M1-M7 red on `80889fd` (DDR-1178 §4). Emulated (TCG) only, not verified on real hardware.
- [ ] P2 SysV shared memory (IPC_PRIVATE only; the shmid is a capability): D1-D5 decided in DDR-1178 Amendment 1 (#70); P2 design is Amendment 2, and the code plus `smoke-shm` arms G-M are in #78. Not verified until CI is green and S2-1 to S2-7 are shown red.
- [ ] P3 POSIX `SHM_ANON` (after P2)
- [ ] P4 shared file mappings with write-back (after P3; D5: SFS `MAP_SHARED|PROT_WRITE` is `-EACCES`)
Exit gate: `smoke-shm` (P1 arms A-F; P2-P4 arms are added with their parts).

### C1.4 Threads and synchronization
Status: IN PROGRESS on C1.3 P1 (Order 4 item 1, reasoning in DDR-1178 Amendment 1). Ref: DDR-1181. Design #73; P1a `struct mm` #74; P1b locks and the uaccess fixup #75; P1c threads plus `smoke-thread` #76 (arms A-G and M; D, E and M are the three DDR-1177 items below). None is merged; mutants T1-T8 are dispatched after #76's correct-code run is green.
- [ ] futex
- [ ] pthreads / `CLONE_VM`, thread-local storage
- [ ] mutex, semaphore, condition variable, barrier (priority inheritance)
- [ ] RCU
- [ ] deadlock detection (lock-order checking)
- [ ] REQUIRED (DDR-1177 Amendment 2, #45/6251086742 item 3): TLB shootdown arm D, `mprotect` read-only on one CPU makes a write on another CPU fault
- [ ] REQUIRED (same): TLB shootdown arm E, the user-space `munmap`/remap stale-read test with `CLONE_VM` threads
- [ ] REQUIRED (same, item 5): a gate that catches DDR-1177 mutant M4 (frame freed before every CPU acked), via a poison-on-free reader thread. Owner: the agent working this checklist. M4 must be shown red.
Exit gate: threads and lock gates (names TBD) incl. race stress, plus the three DDR-1177 items above.

### C1.5 Scheduler classes
Status: TODO. Needs: C1.4.
- [ ] real-time classes (FIFO, RR, deadline)
- [ ] pressure stall information (CPU, memory, I/O)
- [ ] CPU frequency and tickless timers, high-resolution clocks
Exit gate: scheduling-class gate (name TBD).

### C1.6 Dynamic linking and loader
Status: TODO. Needs: C1.3.
- [ ] ELF dynamic loader, relocation, shared libraries, shared text pages
- [ ] user-space ASLR
Exit gate: dynamic-linking gate (name TBD).

### C1.7 KASLR
Status: TODO (was deferred, now in scope). Needs: C0.1. Ref: DDR-1051.
Exit gate: KASLR gate (name TBD).

---

## Phase 2. Memory and storage

### C2.1 Virtual memory extensions
Status: TODO. Needs: C1.3.
- [ ] swap device, page replacement (clock/LRU-approximation), working-set control
- [ ] page cache with dirty-page write-back
- [ ] huge pages, `mlock`
Exit gate: memory-pressure gate (name TBD).

### C2.2 VFS layer and file semantics
Status: TODO. Needs: C2.1.
- [ ] one VFS over FAT32, SFS, ext4; inode and dentry caches; hard links
- [ ] permissions, ACLs, file locking
Exit gate: VFS and permissions gates (names TBD).

### C2.3 Filesystem completion
Status: TODO. Needs: C2.2.
- [ ] SFS large files (> 16 KiB), SFS on-disk free tree. Ref: DDR-1100.
- [ ] ext4 write (journaled)
- [ ] snapshots (copy-on-write), TRIM, I/O scheduler
- [ ] durable on-disk audit log (unblocked by SFS large files)
Exit gate: large-file, ext4-write, snapshot and durable-audit gates (names TBD).

---

## Phase 3. Identity and security

### C3.1 Accounts and authentication
Status: TODO. Needs: C2.2.
- [ ] users, groups, login, password hashing
Exit gate: login gate (name TBD).

### C3.2 Isolation
Status: TODO. Needs: C1.4, C3.1.
- [ ] namespaces (process, network, mount, hostname, IPC, user)
- [ ] cgroups-style resource groups (CPU quota, memory limit, I/O)
- [ ] op-level syscall filtering (account for `io_uring` bypass)
- [ ] mandatory access control hooks
Exit gate: isolation and escape-attempt gates (names TBD).

### C3.3 Cryptographic platform
Status: TODO. Needs: C3.1.
- [ ] disk encryption, TPM support (measured boot, sealed keys)
- [ ] secure boot, code signing of binaries and updates
- [ ] stack canaries, control-flow integrity
Exit gate: encrypted-boot and signed-binary gates (names TBD).

### C3.4 Network security
Status: TODO. Needs: C4.2 (network stack).
- [ ] general stateful firewall and traffic shaping (beyond egress allowlist)
Exit gate: firewall gate (name TBD).

---

## Phase 4. Devices and connectivity

### C4.1 USB
Status: TODO. Needs: C1.4. Ref: xHCI keyboard gated (DDR-1167).
- [ ] hubs, mouse, mass storage, descriptors hardening
Exit gate: usb-hub, usb-mouse, usb-storage gates (names TBD).

### C4.2 Network stack
Status: TODO. Needs: C1.4. Ref: DDR-988, DDR-1104.
- [ ] IPv6, TLS, TAP, completion of deferred net work
Exit gate: ipv6, tls, tap gates (names TBD).

### C4.3 Wireless
Status: TODO. Needs: C4.2, C3.3.
- [ ] Wi-Fi, Bluetooth (honest label: not verified on real hardware if only emulated)
Exit gate: emulated wireless gate (name TBD).

### C4.4 Storage IRQ and audio
Status: TODO. Needs: C1.4.
- [ ] NVMe completion IRQ
- [ ] HDA audio and mixer
Exit gate: nvme-irq, audio gates (names TBD).

### C4.5 GPU, display, power
Status: TODO. Needs: C1.5.
- [ ] GPU acceleration path (virtio-gpu first), multi-monitor
- [ ] power management, suspend, battery
- [ ] touch and accessibility input
- [ ] printing
Exit gate: gpu, suspend, accessibility gates (names TBD).

---

## Phase 5. Platform services

### C5.1 Packages and updates
Status: TODO. Needs: C3.3, C2.3.
- [ ] package manager, content-addressed signed packages, signed atomic updates, rollback
- [ ] configuration store
- [ ] crash dumps
- [ ] i18n and locale
Exit gate: update-and-rollback gate (name TBD).

### C5.2 Virtualization
Status: TODO. Needs: C2.1, C3.2.
- [ ] hypervisor support (VT-x, AMD-V), checkpoint and restore
Exit gate: guest-boot gate (name TBD).

---

## Phase 6. Desktop and branding

### C6.1 Desktop completion
Status: TODO. Needs: C4.5.
- [ ] taskbar magnify, Horizon animated mesh, agent panel graphs (CPU sparkline, memory graph, action histogram)
- [ ] Sovereign/Manual switch polish
Exit gate: extends smoke-mode, smoke-horizon, smoke-agentpanel.

### C6.2 Original branding
Status: TODO. Needs: none (run in parallel).
- [ ] logo, wordmark, icon set from scratch in `assets/brand/`, SVG source, documented grid, stroke and radius
- [ ] no copying or tracing of any existing OS or icon pack; DDR records the rule
- [ ] icon gate: exists, fits grid, renders at 16/24/32/64 px
Exit gate: icon gate (name TBD). Operator reviews by eye before v1.

---

## Phase 7. Agent layer (AETHER)

### C7.1 Agent capabilities
Status: TODO. Needs: C3.2. Ref: Block 3.
- [ ] UI-state reading, input injection
- [ ] sandboxed shell and browser
- [x] rollback on rejection (DDR-1171): #51 `db5d701`, gate `smoke-rollback`. Emulated (TCG) only, not verified on real hardware.
- [ ] kernel policy hooks: agents propose tunings; kernel validates against fixed bounds and audits; agents never run scheduler, memory or security decisions directly
Exit gate: each capability has a gate plus a negative gate.

### C7.2 Domain agents
Status: TODO. Needs: C7.1. Ref: F#66-F#75.
- [ ] domain agents built on `user/agent_base.c`
Exit gate: one gate per agent.

### C7.3 Inference
Status: TODO. Needs: C4.2, C3.2.
- [ ] local inference (Ollama) gated where possible
- [ ] cloud inference (K1, DDR-793 now in scope) with egress and privacy gates
- [ ] quantum interface plus simulator path (K4), no speedup claims
Exit gate: cloud-bridge egress gate, privacy gate.

### C7.4 Adversarial gates
Status: TODO. Needs: C7.1.
- [ ] prompt injection, self-approval, budget overrun, egress bypass, sandbox escape
Exit gate: adversarial gate set.

---

## Phase 8. Architectures and chipset variants

### C8.1 Variants
Status: TODO. Needs: Phase 1 through 5 stable.
- [ ] x86_64 CPU-family variants (runtime dispatch beyond ERMS/NX/SMEP/SMAP/MCE)
- [ ] aarch64 and riscv64 as QEMU-gated builds (record limits)
- [ ] Apple Silicon: only what can be built and tested without hardware; else document as not verified
Exit gate: per-variant boot gate.

---

## Phase 9. Test and harden

### C9.1 Sanitizers
Status: TODO. Needs: Phases 1 to 7.
- [ ] KASAN-style, UBSAN, KCSAN-style race detection, uninitialized-memory checks, all gated

### C9.2 Fuzzing
Status: TODO. Needs: C9.1.
- [ ] coverage-guided syscall fuzzer (syzkaller-style: descriptions, VM executor, corpus, triage)
- [ ] parser and driver fuzzing (ELF loader, filesystems, USB descriptors, network stack, IPC)
- [ ] run until coverage plateaus; record hours

### C9.3 Security sweep
Status: TODO. Needs: C9.2.
- [ ] dependency and secret scanning
- [ ] every bug: root-cause fix, regression gate, mutation check
- [ ] report fuzz hours, bugs found and fixed, anything still open; never claim 'no vulnerabilities'

---

## Phase 10. Release

### C10.1 v1 ISO
Status: TODO. Needs: every chunk except C11.1 DONE, or recorded as an honestly-labelled exception.
- [ ] one release SHA, full regression green
- [ ] VirtualBox install-and-boot gate green
- [ ] readiness report on issue #45: every feature labelled gated / emulated-verified / not verified on real hardware
- [ ] tag v1.0.0, promote `dev/phase1` to `main`, attach ISO and SHA-256 to the release

---

## Phase 11. Last in build order (operator rule)

### C11.1 Rust and build system (was C8.2)
Status: TODO. Needs: C10.1. Always built last.
- [ ] Rust only where a DDR shows a measured safety or speed win
- [ ] Makefile/CMake hybrid only if a DDR justifies it
- [ ] Because this lands after hardening and the v1 tag: any merged change re-runs the C9.1 to C9.3 gates and the C10.1 release gate, and ships as a patch release (v1.0.x), never silently
Exit gate: DDR with measured results; C9 and C10 gates green again on the new SHA.
