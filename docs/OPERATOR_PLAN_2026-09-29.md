# Operator plan: decisions and ownership (updated 2026-10-03, rev 2)

The requirements, ports, touch and display work, and the missed-components register are in `docs/T1_REQUIREMENTS_REGISTER.md`. This file holds decisions, hardware facts, hunt methods and PR ownership.

Tags: **[APPROVED]** the operator approved it (2026-10-03 messages at 14:29, 14:34 and 19:32 +04); **[PROPOSED]** a recommendation the operator has not approved, so it must not be treated as decided; **[VERIFIED]** read from this repo, measured, or stated by the operator; **[QUESTION]** the implementer must answer from the code; **[EXTERNAL]** a public source, linked inline; **[INFERENCE]** my reasoning, not verified.

## 1. Milestones

- **T1** is the first test ISO. It must install and boot in VMware Workstation on the operator's Lenovo Legion (BIOS and UEFI) with display, keyboard, mouse, install to an encrypted disk, network, a live agent and a UI shell. Touch is validated on emulated devices, because the operator owns no touch hardware.
- **v1.0.0** is the release. Its gates are in `docs/PRE_LAUNCH_CHECKLIST.md` section 0 and are unchanged by this file.
- **POST** means after release.

## 2. Decisions

### Approved by the operator

| ID | Decision |
|---|---|
| D1 | [APPROVED] No universal vendor escrow in v1. |
| D2 | [APPROVED] One random, high-entropy, user-held recovery key, shown once, with a checksum; the installer forces proof that it was saved. Shamir splitting is POST. |
| D3 | [APPROVED] The escrow backend and identity checks are an operator task outside this repo. |
| D4 | [APPROVED] TPM+PIN is an optional UEFI unlock. Never TPM-only. The sealing policy is the one in DDR-1145 section 3 (PCRs 0, 4, 7 and 9; PCR 9 is there because the kernel on the unencrypted ESP is not measured by the firmware PCRs). The earlier wording in this file, "PCR 7 first", is withdrawn. |
| D5 | [APPROVED] Follow DDR-1143 section 8.1: Argon2id if it passes known-answer tests before the encryption code freezes; otherwise PBKDF2-HMAC-SHA256 at 600,000 iterations or more, with the weakness stated in the release notes. Either way the keyslot header carries a KDF id, and a PBKDF2 slot is re-wrapped to Argon2id on the next successful unlock once Argon2id is available. |
| D6 | [APPROVED] BIOS installs are passphrase-only. |
| Scope | [APPROVED] USB, touch and multi-display are in T1. Native GPU drivers are POST. |
| TPM fallback | [APPROVED] Build TPM+PIN last. If it slips, v1 may ship passphrase plus recovery only, with the gap stated in the release notes. |
| Secure Boot | [APPROVED] Unsupported in v1 and documented. Tamper detection of the kernel comes from the DDR-1145 PCR policy, not from Secure Boot; a signed boot chain is POST. [QUESTION] Claude Code to confirm from DDR-1145 exactly which tampering the PCR 9 policy detects. |
| Keyslots | [APPROVED] Layered: passphrase (mandatory), TPM+PIN (optional, UEFI), recovery key (always created). |
| OPEN-2 bar | [APPROVED] The 11,000-boot QEMU campaign clean, plus an installed-disk hunt and a hardware-accelerated (VMware) hunt. Each residual signature gets a named mechanism with a test, or an honest written non-closure. |
| OPEN-1 | [APPROVED] Route 1 may ship named-open only if the installed-disk and VMware hunts are also clean. Any hit in them blocks the tag. |

Supersession: where this file differs from DDR-1143 section 8 or DDR-1145 section 3, the sentence above states which governs. Claude Code is to add a one-line pointer in each DDR to this file.

### Proposed, awaiting the operator

- [PROPOSED] Wrap the ledger seed in P2 sector 1 under the volume key as part of DDR-1144, because it is currently readable from the disk without the passphrase [VERIFIED by Claude Code, DDR-1153 section 6].
- [PROPOSED] Accept that rollback of an older valid sector is not detected in v1, and say so in the release notes (DDR-1144 threat table). Revisit with the TPM monotonic counter in DDR-1145.
- [PROPOSED] No KCSAN-style race checker before v1; revisit after v1 as a debug-flag build exercised by the open2-hunt workflow.

## 3. Operator hardware [VERIFIED by the operator]

- Lenovo Legion laptop, not a touchscreen. VMware Workstation is installed.
- External monitor: LG 24 inch, 100 Hz, NVIDIA G-Sync, not a touchscreen.
- [INFERENCE] G-Sync is an NVIDIA feature, so the monitor is probably wired to the NVIDIA GPU. Driving it on real hardware would then need an NVIDIA driver, which is why native external display is POST. G-Sync itself is variable refresh and is not needed for the monitor to work as a plain display.
- Consequences: T1 proves multi-display in VMware; touch is tested on emulated devices; real touch validation waits until a USB touch device is available.

## 4. OPEN-1 and OPEN-2 methods

- [EXTERNAL] QEMU record/replay needs icount and single-threaded TCG. It does not model parallel vCPU execution, so it does not fit SMP races that need parallel execution (https://www.qemu.org/docs/master/system/replay.html, https://www.qemu.org/docs/master/devel/tcg-icount.html). Do not invest in it for those -smp hangs.
- [VERIFIED by Claude Code] An NMI-based capture of a frozen CPU already exists: DDR-981 `ap_freeze_probe` stashes the frozen CPU's RIP and backtrace, and DDR-1138 adds one NMI per additional frozen CPU. No new DDR is needed.
- [VERIFIED] DDR-1130: the QEMU campaign boots without a filesystem. Add an installed-disk hunt and a VMware hunt.
- DDR-1155 (U1) is a hypothesis: a freed TCB poisoned by KASAN. Write a short factual record first, then a use-after-free regression test of our own kernel. U1, U2 and U3 stay open.
- [VERIFIED] Campaign status (DDR-1154): 5,445 of 11,000 boots clean on kernel 182c30bb16930d57, 95% upper bound about 0.055% per boot. Dispatch 5 lane 9 timed out and is counted as neither clean nor a signal.

## 5. Open-PR ownership

| PR | Owner | Action |
|---|---|---|
| #19 | Dependabot, then operator | Rebase requested; merge when CI is green and the patched fast-uri version is confirmed. |
| #9 | Dependabot | Recreate requested; merge the refreshed PR if CI is green. |
| #7, #8 | Claude Code | Merge after the v1.0.0 tag (they touch ci.yml). |
| #27 | Operator | Review; merge when satisfied. |
| #29 | Claude Code | DDR-1154 draft; mark ready after CI is green. |
| Dependabot alert #24 | Claude Code | Triage done: low for this repo; state the patched versions before any change. |
