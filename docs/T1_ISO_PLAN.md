# T1 ISO plan — unbuilt work per layer L1–L7

**Purpose.** PR #27 (operator, 2026-10-03) asked for a plan that enumerates
**every unbuilt item per layer**, each tagged **T1** (needed for the VMware test
ISO), **v1** (needed for release), or **POST** (after release). This file is the
by-layer view; the authoritative requirement text, decisions and external
sources live in `docs/OPERATOR_PLAN_2026-09-29.md` and
`docs/T1_REQUIREMENTS_REGISTER.md` on branch `docs/operator-plan-2026-09-29`
(commit `0ad164c`). Where this file and the register disagree, **the register
governs**; this file only re-sorts its items by layer and points back to the
`R-*` / `#nn` IDs.

**Milestones.** T1 = the VMware-on-Legion test ISO (register §6 acceptance). v1
= the tagged release. POST = after `v1.0.0`.

**What this plan does NOT do.** It does not re-decide anything. Items the plan
marks `[PROPOSED]` stay PROPOSED and need operator approval before build (plan
§"PROPOSED"). The `v1.0.0` tag, the `main` promotion and every merge remain
operator decisions. Nothing here is a commitment to build before its DDR exists;
each T1 driver item below is "behind its own DDR and gate" per PR #27 item 5.

**How "unbuilt" was decided.** Each row is either VERIFIED against the tree
(a grep/read in this repo) or carried from the register with its tag. Rows that
are already shipped are listed under "Already built" at the end of their layer so
the plan is not read as re-doing them.

---

## L1 — Boot / firmware entry

Already built: BIOS stage1/stage2 (ADR-001/002), UEFI loader + GOP framebuffer
(#15/#5, OVMF-verified only), ADR-034 aarch64/riscv64 boot stubs, the DDR-1143
pristine-kernel copy and `boot_info`, ADR-034 ISO (x86 only).

| Item | Why | Tag |
|---|---|---|
| **UEFI boot verified under VMware firmware** (not just OVMF) — GOP mode set, `EFI_TCG2_PROTOCOL.LocateProtocol`, handoff | T1 acceptance is "both BIOS and UEFI in VMware" (register §6); GOP + TCG2 are OVMF-verified only (#15, DDR-1142/1145 §1) | **T1** |
| **BIOS boot verified under VMware** | same acceptance line; SeaBIOS vs our stage1/stage2 INT 13h path untested on VMware's BIOS | **T1** |
| EDID read + more than one scanout (today one scanout, 1024×768 fallback, #55) | register R-D1 | **T1** |
| Document GOP-only second-display behaviour and what needs a native GPU driver | register R-D4 | **T1** |
| Measured-kernel chain on real UEFI hardware (dTPM bus exposure, DDR-1145 §6) | needs physical TPM | **POST** |

## L2 — Kernel core (mm, sched, traps, caps)

Already built: PMM/VMM/COW/KASAN poison, SMP + per-CPU runqueues, W^X incl.
identity alias (DDR-1046) + CR0.WP (DDR-1126), SMEP/SMAP (DDR-1040/1041), #MC
(DDR-1044), the DDR-1139 double-dispatch fix, the capability system.

| Item | Why | Tag |
|---|---|---|
| **DDR-1155 KASAN use-after-free regression test** (U1/U2/U3) | owed; §4 of DDR-1155 marks it PENDING | **v1** |
| OPEN-2 campaign to 11,000 boots (DDR-1154) | the one open release blocker; 5,445/11,000 read, 0 signals | **v1** |
| Cross-CPU TLB shootdown — prerequisite of `CLONE_VM`/pthreads, not a Phase-9 optimisation (DDR-1075 §3, guarded by `ci-cr3-writers-check`) | blocks L5 pthreads | **v1** (only if pthreads is pulled into v1; else POST) |
| KPTI / retpoline / RSB | assessed, recommended deferral (DDR-1140); TCG cannot demonstrate it | **POST** |
| KASLR | assessed, deferred as a sequencing call (DDR-1051) | **POST** |

## L3 — Drivers (bus, block, net, input, display, audio, TPM)

Already built: PCIe, MSI-X, virtio-blk/-net/-input/-rng, AHCI, NVMe (poll),
e1000e, DDR-1143 partition sub-device + MBR, block flush op, DHCP/DNS
(DDR-1141), the DDR-1148 vblk timeout instrument.

| Item | Why | Tag |
|---|---|---|
| **xHCI host controller + USB hub + HID class** (incl. multi-touch digitizer report: contact id, position, pressure) | register R-T1; "no USB code" (#16); T1 touch needs it for `usb-tablet`/`usb-wacom-tablet` | **T1** |
| **I2C-HID via ACPI** (laptop touchscreens, precision trackpads) | register R-T2 | **T1** |
| **TPM 2.0 transport (TIS + CRB), seal/unseal, PCR policy {0,4,7,9}** | DDR-1145; swtpm+OVMF gateable; needed for encrypted-install acceptance | **T1** (logic in QEMU/VMware) / **POST** (dTPM bus) |
| HD Audio controller + codec (incl. HDMI/DP audio) | register R-01 / R-02 jack; no audio stack | **v1** |
| Realtek RTL81xx + Intel I225/I226 2.5GbE NICs | register §1; real-hardware Ethernet | **v1** |
| USB mass storage + hot-unplug flush safety | register R-08 | **v1** |
| Native GPU modesetting (the port-wiring problem, register §1 INFERENCE) | real external displays, refresh/VRR (R-D5) | **POST** |
| USB UVC webcam + microphone | register R-09 | **POST** |
| Wi-Fi / Bluetooth per-chipset drivers + firmware (#38/#39) | register §1 | **POST** |
| USB-C power/roles (UCSI), Thunderbolt/USB4, SD reader, print/scan | register §1 / R-16 | **POST** |
| ACPI battery/AC via EC, lid switch, brightness keys, suspend | register R-06 | **v1** |
| Thermal/fan reporting (#54) | register R-07 | **POST** |

## L4 — Filesystem / install / encryption

Already built: SFS (dirs, unlink, B+tree, GC, cross-reboot persistence), FAT32
r/w, ext4 read, mkfs.sfs, DDR-1143 install syscalls + FAT16 + installer +
root-selection + `smoke-install`, DDR-1153 ledger seed in P2 sector 1.

| Item | Why | Tag |
|---|---|---|
| **DDR-1144 crypt device** — ChaCha20-Poly1305 AEAD, per-volume DRBG nonce, HKDF per-block subkeys, A/B superblock+journal, KDF per D5 (Argon2id-or-PBKDF2), known-answer tests | register §4, build order step 1 | **T1** |
| **DDR-1146 recovery key** | register §4, order step 2; "never hard-fail" fallback | **T1** |
| **DDR-1145 TPM+PIN unlock** (never TPM-only, D4) | register §4, order step 3 | **T1** |
| Key zeroize on shutdown/panic; keys never in klog; entropy-confirmed-or-fail-closed; passphrase min length + lockout + re-wrap | register §4 "other requirements" | **T1/v1** (T1: fail-closed + zeroize; v1: lockout UI) |
| Keyslot header redundancy + 4Kn disk test | register §4 | **v1** |
| `[PROPOSED]` rollback-of-older-valid-sector detection; wrap the P2 ledger seed under the VMK | register §4 / plan proposed list — **needs operator approval** | **PROPOSED** |

## L5 — Userspace / syscalls / net stack / shell

Already built: ELF loader + W^X, POSIX syscalls, pipes/epoll/signals/io_uring
(baseline)/musl, PRISM shell (pipes, redirection, quoting, job control, `wait`,
`source`), lwIP TCP + the egress allowlist + audit, `SYS_MPROTECT`, `SYS_POLL`,
file-backed `MAP_PRIVATE` mmap, DHCP/DNS ring-3 door.

| Item | Why | Tag |
|---|---|---|
| **Multi-contact input abstraction (`ABS_MT_*` slots)** — tap/drag/pinch, palm rejection, on-screen keyboard | register R-T3; `virtio_input.c` folds only ABS_X/ABS_Y today | **T1** |
| **Point the agent at a VMware-reachable Ollama** (NAT ≠ QEMU's 10.0.2.2) | register R-M4 | **T1** |
| File manager + settings app | register R-10 | **T1** |
| Browser / web view + TLS cert store (TLS shim: primitives in-tree, needs record layer + X.509 + trust anchor, DDR-1104/1059) | register R-11 | **v1** |
| IPv6 ring-3 door (transport flag + v4-shaped allowlist widening, DDR-1104 §2), firewall, network settings UI | register R-12 | **v1** |
| Unicode fonts, IME, non-US layouts (#56), emoji fallback; locale/time/NTP | register R-03/R-04 | **v1** |
| Model-source layer R-M1/R-M2/R-M3 (cloud key encrypted under VMK, signed local-model slot, all via CAP_NET+audit) — **DDR first** | register §5, PR #27 item 7 | **v1** (merge after tag) |
| `SYS_FUTEX` / pthreads (blocked on L2 shootdown + file-backed-or-shared mmap, DDR-1038/1075) | register (threading) | **v1/POST** |
| Clipboard, drag-and-drop, text editor | register R-10 | **v1** |
| Signed update + rollback (#20); crash reporting w/o key material + persistent log (#41/#42); diagnostic bundle (R-22) | register R-13/R-14/R-22 | **v1** |

## L6 — AETHER / agent layer

Already built: the daemon, Section 3C action types (8/8), Section 3D (#45–#65),
the metric lockbox, the tamper-evidence audit chain (F#76 evidence half),
DDR-1149 CAP_OCR/CAP_SCENE/CAP_NET_BROWSE at submission, DDR-1150 signed ledger
head (Route 3).

| Item | Why | Tag |
|---|---|---|
| Point agents at a reachable model source (ties to L5 R-M4) | T1 acceptance: "the agent answers through a local Ollama server" | **T1** |
| Durable audit ledger (F#76 durability half) — the flusher blocked on the write-side extent ceiling (DDR-1098/1100) | register R-14 adjacent | **v1** |
| Domain agent behaviours F#66/67/69–75 (OCR model, scene graph, cloud bridge) | caps granted (DDR-1149), behaviours not built; cloud bridge is `aether/cloud_bridge/transport.py` (Python, deferred, DDR-793) | **POST** |

## L7 — Compositor / desktop / UI

Already built: the framebuffer compositor, window lifecycle, resize/close, dock,
maximize, Alt-Tab, Super+M, Ctrl+Alt+T terminal, OKLab horizon bands, the
DDR-1147 palette engine + Regalia/Consort toggle, live agent-metrics panel
(plumbing), the compositor's shown agents + approval queue.

| Item | Why | Tag |
|---|---|---|
| **UI hit targets + layouts usable for finger AND pointer at once** | register R-T4; PR #27 item 8 | **T1** |
| **Multi-scanout / second-display add-and-remove in the compositor** (hotplug, extended/mirrored, per-monitor resolution) | register R-D2; T1 acceptance "a second display can be added and removed" | **T1** |
| Bind each touch device to its display + per-display calibration | register R-T5 | **v1** |
| Per-monitor scaling from EDID physical size | register R-D3 | **v1** |
| Accessibility: keyboard-only, screen reader, high contrast, large text, sticky keys | register R-02 | **v1** |
| Agent-metrics visualisations (CPU sparkline, memory graph, action-rate histogram — plumbing shipped, views not) | DDR-1072 §5 | **v1** |
| Validate touch on a real USB touch monitor | register R-T6 | **POST** |

---

## T1 acceptance checklist (register §6, verbatim intent)

In VMware on the Legion, **BIOS and UEFI**, report pass/fail per item:

1. Install to an encrypted disk succeeds.
2. Passphrase and recovery unlock work.
3. Display, keyboard, mouse work.
4. Network gets an address by DHCP.
5. The agent answers through a local Ollama server.
6. The UI shell shows agents and the approval queue.
7. (QEMU or VMware, emulated) multi-touch contacts and gestures work via `virtio-multitouch-pci`.
8. (QEMU or VMware, emulated) a second display can be added and removed.

## Build order inside T1 (dependencies first)

1. L4 encryption: DDR-1144 → DDR-1146 → DDR-1145 (register §4 order; TPM needs the KDF + keyslots under it).
2. L3 xHCI+HID and I2C-HID → L5 `ABS_MT_*` abstraction → L7 finger/pointer UI (touch chain).
3. L3 EDID/multi-scanout → L7 second-display add/remove (display chain).
4. L5 VMware-reachable Ollama (R-M4) → L6 agent answers (depends only on NAT address).
5. L1 VMware BIOS+UEFI boot verification — last, because it validates the assembled ISO.

Each L3/L5 driver item lands **behind its own DDR and gate** (PR #27 item 5).
Install/encryption and the model-source layer land as DDRs first (PR #27 items
6, 7); the model-source layer **merges after the `v1.0.0` tag** (PR #27).
