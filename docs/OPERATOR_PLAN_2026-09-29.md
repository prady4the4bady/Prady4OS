# Operator plan: decisions and ownership (updated 2026-10-03)

The requirements, ports, touch and display work, and the missed-components register are in `docs/T1_REQUIREMENTS_REGISTER.md`. This file holds decisions, hardware facts, hunt methods and PR ownership.

Tags: **[APPROVED]** the operator approved it (2026-10-03, 14:29 +04 and 14:34 +04 messages); **[VERIFIED]** read from this repo or stated by the operator; **[QUESTION]** the implementer must answer from the code; **[EXTERNAL]** a public source, linked inline; **[INFERENCE]** my reasoning, not verified.

## 1. Milestones

- **T1** is the first test ISO. It must install and boot in VMware Workstation on the operator's Lenovo Legion (BIOS and UEFI) with display, keyboard, mouse, install to an encrypted disk, network, a live agent and a UI shell. Touch is validated on emulated devices, because the operator owns no touch hardware.
- **v1.0.0** is the release. Its gates are in `docs/PRE_LAUNCH_CHECKLIST.md` section 0 and are unchanged by this file.
- **POST** means after release.

## 2. Decisions [all APPROVED by the operator]

| ID | Decision |
|---|---|
| D1 | No universal vendor escrow in v1. |
| D2 | One random, high-entropy, user-held recovery key, shown once, with a checksum; the installer forces proof that it was saved. Shamir splitting is POST. |
| D3 | The escrow backend and identity checks are an operator task outside this repo. |
| D4 | TPM+PIN is an optional UEFI unlock. Never TPM-only. |
| D5 | PBKDF2-HMAC-SHA256 at 600,000 iterations or more for v1, with a KDF id in the keyslot header and a re-wrap to Argon2id on the next successful unlock once Argon2id passes known-answer tests. |
| D6 | BIOS installs are passphrase-only. |
| Scope | USB, touch and multi-display are in T1. Native GPU drivers are POST. |
| TPM | Bind TPM sealing to PCR 7 first; evaluate PCRs 11 and 14. Build TPM+PIN last. If it slips, v1 may ship passphrase plus recovery only, with the gap stated in the release notes. |
| Secure Boot | Unsupported in v1 and documented, so TPM unlock protects against a stolen disk, not a tampered loader. |
| Keyslots | Layered: passphrase (mandatory), TPM+PIN (optional, UEFI), recovery key (always created). |
| OPEN-2 bar | The 11,000-boot QEMU campaign clean, plus an installed-disk hunt and a hardware-accelerated (VMware) hunt. Each residual signature gets a named mechanism with a test, or an honest written non-closure. |
| OPEN-1 | Route 1 may ship named-open only if the installed-disk and VMware hunts are also clean. Any hit in them blocks the tag. |

Nothing in this file is awaiting a decision. Future changes need a new operator comment.

## 3. Operator hardware [VERIFIED by the operator]

- Lenovo Legion laptop, not a touchscreen. VMware Workstation is installed.
- External monitor: LG 24 inch, 100 Hz, NVIDIA G-Sync, not a touchscreen.
- [INFERENCE] G-Sync is an NVIDIA feature, so the monitor is probably wired to the NVIDIA GPU. Driving it on real hardware would then need an NVIDIA driver, which is why native external display is POST. G-Sync itself is variable refresh and is not needed for the monitor to work as a plain display.
- Consequences: T1 proves multi-display in VMware; touch is tested on emulated devices; real touch validation waits until a USB touch device is available.

## 4. OPEN-1 and OPEN-2 methods

- [EXTERNAL] QEMU record/replay needs icount and single-CPU TCG, so it does not fit SMP races (https://www.qemu.org/docs/master/system/replay.html, https://www.qemu.org/docs/master/devel/tcg-icount.html). Do not invest in it for -smp hangs.
- [EXTERNAL] Linux detects hard lockups with an NMI per CPU (https://www.kernel.org/doc/html/latest/admin-guide/lockup-watchdogs.html). [QUESTION] Would an NMI-based cross-CPU check capture a frozen CPU's RIP with interrupts masked? It needs its own DDR and touches the DDR-1079 path.
- [EXTERNAL] KCSAN uses sampled watchpoints (https://docs.kernel.org/dev-tools/kcsan.html). [QUESTION] Is a cheap analogue worth the cost?
- [VERIFIED] DDR-1130: the QEMU campaign boots without a filesystem. Add an installed-disk hunt and a VMware hunt.
- DDR-1155 (U1) is a hypothesis: a freed TCB poisoned by KASAN. Add a deliberate-poison test that reproduces the signature. Do not claim closure.

## 5. Open-PR ownership

| PR | Owner | Action |
|---|---|---|
| #19 | Dependabot, then operator | Comment @dependabot rebase; merge when CI is green. |
| #9 | Dependabot | Comment @dependabot recreate. |
| #7, #8 | Claude Code | Merge after the v1.0.0 tag (they touch ci.yml). |
| #27 | Operator | Review; merge when satisfied. |
| dev/phase1-seyp3n | Claude Code | Open a PR for DDR-1154 and DDR-1155. |
| Dependabot alert #24 | Claude Code | Triage; report before acting. |
