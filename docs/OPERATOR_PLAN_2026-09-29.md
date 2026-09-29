# Operator plan, 2026-09-29: what is left before the v1.0.0 tag

Status: PROPOSAL. Nothing here is a decision until the operator approves it by comment or merge.
Every line is tagged: **[VERIFIED]** read from this repo, **[PROPOSED]** a recommendation, **[QUESTION]** something the implementer must answer from the code, or **[EXTERNAL]** taken from a public source (linked in section 8). I have not read the full body of DDR-1144/1145/1146; the QUESTION items exist because of that.

## 1. Where things stand

- [VERIFIED] `docs/PRE_LAUNCH_CHECKLIST.md` lists one GATING ITEM: installer, persistent encrypted root, TPM unlock, recovery escrow. Its text says DESIGNED, NOT BUILT. Blocks the tag.
- [VERIFIED] Checklist #40 records the 2026-09-25 decision (comment 5839562349): full-volume encryption is in v1 scope, not deferred. Designs: DDR-1144 (encryption), DDR-1146 (recovery escrow).
- [VERIFIED] DDR-1143 section 8 holds open decisions D1 to D6 (escrow custody, escrow transport, escrow backend, TPM-only vs TPM+PIN, passphrase KDF, BIOS passphrase-only).
- [PROPOSED] Track status: installer built (DDR-1143 pieces 4 to 6 and the ledger-seed persistence follow-up); encryption, TPM unlock and escrow are design only; OPEN-1 route 1 has no captured evidence; OPEN-2 has a strong fix with residual signatures. The checklist text for the installer should be refreshed to match.

## 2. Proposed decisions D1 to D6

| ID | Proposal | Why |
|---|---|---|
| D4 | TPM+PIN as an optional UEFI unlock. Never TPM-only. | TPM-only is weaker than adding a second factor; bus sniffing and DMA/cold-boot are documented [EXTERNAL 1,2,3]. There are no user accounts, so the PIN is the only knowledge factor. |
| D5 | PBKDF2-HMAC-SHA256 at 600,000 iterations or more for v1, weakness stated in release notes. Keyslot header carries a KDF id so Argon2id can be added later without a format break. | OWASP accepts PBKDF2 at that setting and prefers Argon2id [EXTERNAL 4]. Argon2id needs BLAKE2b, a new primitive that needs known-answer tests. Measure boot time first (see 5.7). |
| D6 | Accept passphrase-only on BIOS installs. | TPM2 sealing depends on UEFI measurements [EXTERNAL 5]. |
| D1 | Do not ship universal vendor escrow in v1. | DDR-1143 D1 notes that any escrow able to recover every device holds a credential that decrypts every record. |
| D2 | User-held recovery record (printed or typed text) for v1. Network upload after a backend exists. | Matches the existing D2 default. |
| D3 | Out of scope for the OS repo. Record it as an operator task. | External to this OS. |
| Unlock model | Layered: passphrase keyslot is mandatory; TPM+PIN keyslot optional on UEFI; recovery keyslot always created. | LUKS-style multi-keyslot design lets methods be added and revoked independently [EXTERNAL 6]. |

## 3. Components that may have been missed (new)

### 3.1 Boot-chain integrity (evil maid)
- [EXTERNAL] Disk encryption does not protect an unencrypted boot path; pre-boot authentication plus a locked boot path is the standard defence [EXTERNAL 7,8].
- [VERIFIED, earlier session] The ESP (FAT16) carries the loader and kernel in plaintext.
- [QUESTION] What does the threat model say about a tampered loader capturing the passphrase? If the answer is "accepted for v1", say so in the release notes.
- [QUESTION] The go/no-go recommendation was to document Secure Boot as disabled for v1. TPM PCR 7 reflects Secure Boot state [EXTERNAL 9]. Which PCRs does DDR-1145 seal to, and is sealing meaningful with Secure Boot off?

### 3.2 Per-sector AEAD design (DDR-1144)
- [EXTERNAL] XTS gives no authentication; AEAD does, but nonce reuse under the same key is catastrophic for ChaCha20-Poly1305 [EXTERNAL 10,11].
- [QUESTION] How is the per-sector nonce chosen and persisted? Is it derived from (sector, write counter)? What happens to nonce and tag if power fails mid-write (torn write)?
- [QUESTION] Are sector number and key id bound as associated data, so ciphertext cannot be moved between sectors?
- [QUESTION] Replay of an older valid sector version is not stopped by per-sector AEAD. Is that accepted and written down?
- [QUESTION] Where do tags live, and what is the usable-capacity overhead? Does the SFS journal ordering still hold once tag updates are added?

### 3.3 Key lifetime in RAM
- [PROPOSED] Zeroize the volume key on shutdown and on every panic path. Keys must never reach klog.
- [VERIFIED] Checklist #18: S3 suspend is refused, so suspend-key-wipe is not needed today. [EXTERNAL] Linux had a regression where keys stayed in RAM across suspend [EXTERNAL 12]. Add a note to any future S3 DDR that the key wipe is a precondition.
- [VERIFIED] Checklist #41: crash dumps to disk are post-1.0. Add a precondition: a dump writer must never include key material.
- [QUESTION] Is the IOMMU/DMA-protection state known? A DMA-capable device could read the key from RAM.

### 3.4 Entropy at install
- [VERIFIED] The checklist (#30) records that crypto refuses to run without a hardware entropy source.
- [QUESTION] Does the installer generate the volume key and salt only after the entropy source is confirmed, and does it fail closed with a clear message on hardware lacking RDSEED and virtio-rng?

### 3.5 Passphrase handling
- [PROPOSED] Enforce a minimum length. State it in the installer prompt.
- [PROPOSED] Delay or lock out after repeated wrong passphrases at boot. With PBKDF2 (not memory-hard) the passphrase is the weak point of a stolen disk.
- [PROPOSED] Changing the passphrase must re-wrap the volume key, not re-encrypt the disk. Use a key-encryption-key design, one purpose per key [EXTERNAL 13].

### 3.6 Recovery and TPM-drift handling
- [EXTERNAL] Sealing to PCRs breaks when firmware or Secure Boot variables change; a dbx update locked users out of auto-unlock [EXTERNAL 9,14].
- [PROPOSED] On TPM unseal failure, fall back to passphrase prompt, then recovery record. Never hard-fail.
- [PROPOSED] Create the recovery record before encryption is enabled, and force the user to prove they saved it (retype a portion). Escrow before enforcement [EXTERNAL 15].
- [PROPOSED] Tests must exercise recovery, not just creation.

### 3.7 KDF timing under emulation
- [PROPOSED] Measure PBKDF2 at 600,000 iterations in the boot path under QEMU TCG, and on real hardware. If boot delay is unacceptable, tune iterations per install and store the count in the keyslot.

### 3.8 Header and metadata
- [QUESTION] Ledger seed persistence: DDR-1153 stores the ledger seed in P2 sector 1, outside the filesystem, and the volume header is plaintext. After encryption lands, can the seed be read without the passphrase? If yes, wrap it under the volume key or keyslot.
- [PROPOSED] Keep two copies of the keyslot header, or a checksum plus backup, so one corrupt sector does not make the disk unrecoverable.
- [QUESTION] 4Kn (4096-byte sector) disks: does the installer, the partition writer and the crypt device handle them?

### 3.9 Installer safety
- [PROPOSED] Wrong-disk protection: refuse to write to any disk that already holds a partition table unless the operator passes an explicit confirm string naming the device and size.
- [PROPOSED] Power loss during install must leave the disk either untouched or clearly incomplete, never bootable-but-half-encrypted.
- [PROPOSED] First real-hardware installs go to a spare disk with other drives unplugged.

## 4. Test matrix to require before the tag

| Axis | Cases |
|---|---|
| Firmware | BIOS, UEFI (OVMF), one real UEFI machine (the operator's Lenovo Legion) |
| Unlock | passphrase, TPM+PIN (swtpm), recovery record |
| Failure | wrong passphrase, wrong PIN, TPM cleared, PCR changed (simulate firmware update), corrupt header, missing header copy |
| Power loss | during install, during first encrypted write, during passphrase change |
| Disk | virtio, NVMe, AHCI, 512 and 4096-byte sectors, disk too small, disk already partitioned |
| Lifecycle | passphrase change, recovery-key rotation, reinstall over an encrypted disk |
| Security | key absent from klog and panic output, key zeroized after shutdown, ledger seed not readable without unlock |

## 5. OPEN-1 and OPEN-2

### 5.1 OPEN-2 closure bar
- [VERIFIED, earlier session] Strong fix in, 3 residual signatures unexplained.
- [PROPOSED] Each residual signature either gets a named mechanism with a test, or an honest written non-closure in the release notes.
- [PROPOSED] Pick a confidence target up front. Zero failures in N clean boots bounds the per-boot failure rate at about 3/N with 95% confidence (rule of three): 2,200 boots gives about 0.14%, 10,000 boots about 0.03%. The operator picks N.

### 5.2 Hunt the installed configuration
- [VERIFIED] DDR-1130 records that the hunt campaign boots without a filesystem.
- [PROPOSED] The installed persistent-root path adds new disk and crypto work at boot. Run a hunt variant on that path, since the earlier hunts may not exercise it. [QUESTION] Does any current hunt boot the installed disk?

### 5.3 OPEN-1 route 1
- [VERIFIED, earlier session] No captured artefact. The recorded stopping point is SYSFSTAT OK with mnt_lock visible.
- [PROPOSED] Instrument first, then hunt. Tag decision stays with the operator. My recommendation: do not block the tag on route 1 alone if the release notes name it as open and the installed-disk hunt in 5.2 is clean.

### 5.4 Ideas from public sources to evaluate (not decisions)
- [EXTERNAL] QEMU record/replay gives deterministic replay, but it is tied to icount and single-CPU TCG, so it does not fit SMP races [EXTERNAL 16,17]. Do not invest in it for `-smp` hangs. It may still help single-CPU variants.
- [EXTERNAL] Linux detects hard lockups with an NMI on each CPU and soft lockups with a per-CPU timer heartbeat [EXTERNAL 18]. [QUESTION] The `[apfreeze]` detector only prints from a CPU that is still running. Would an NMI-based cross-CPU check capture the frozen CPU's RIP and registers even with interrupts masked? This touches the same fragile path as DDR-1079, so it needs its own DDR and the operator's approval; it is separate from the deferred watchdog decision.
- [EXTERNAL] KCSAN finds races by sampling watchpoints with compiler instrumentation [EXTERNAL 19]. It is Linux-specific. [QUESTION] Is a cheap analogue (sampled watchpoints on the known shared fields) worth its cost under NON-NEGOTIABLE 3?

## 6. Release hygiene carried over from the go/no-go
These are existing checklist items, listed so nothing is lost: licence and third-party notices, ISO checksums and signing, Wi-Fi, Bluetooth, DHCP/DNS (#38, #39, #43: the address is hard-coded to the QEMU slirp address), crash dumps (#41), persistent log (#42), update mechanism (#20), power management (#18). The release notes must state each one that is deferred.

## 7. What this PR does not do
It changes no kernel, build, CI or test file. It approves nothing on its own. It does not restate the DDR bodies.

## 8. Sources ([EXTERNAL n])
1. Microsoft Learn, BitLocker countermeasures: https://learn.microsoft.com/en-us/windows/security/operating-system-security/data-protection/bitlocker/countermeasures
2. Microsoft Learn, TPM fundamentals: https://learn.microsoft.com/en-us/windows/security/hardware-security/tpm/tpm-fundamentals
3. TPM Explained, State of Surveillance: https://stateofsurveillance.org/articles/technical/tpm-trusted-platform-module-explained/
4. OWASP Password Storage Cheat Sheet: https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html
5. TPM and LUKS notes: https://cmspam.github.io/cache22/boot-and-security/tpm-luks/
6. Open Source For You, LUKS keyslots: https://www.opensourceforu.com/2026/09/is-your-organisations-data-secure/
7. SANS ISC, evil maid: https://isc.sans.edu/diary/29256
8. Kicksecure, evil maid: https://www.kicksecure.com/wiki/AEM
9. Ubuntu Discourse, dbx update breaks TPM unlock: https://discourse.ubuntu.com/t/after-uefi-dbx-firmware-update-tpm-auto-unlock-doesnt-work-anymore/62730
10. AES-XTS overview: https://quantumsequrity.com/blog/aes-xts-disk-encryption
11. AEAD nonce reuse: https://p42.studio/en/insights/the-aead-trap-why-nonce-reuse-destroys-symmetric-security
12. LUKS suspend regression: https://mehdirahmani.fr/en/luks-suspend-linux-6-9-encryption-keys-ram/
13. OWASP Key Management Cheat Sheet: https://cheatsheetseries.owasp.org/cheatsheets/Key_Management_Cheat_Sheet.html
14. LUKS TPM2 sealing: https://www.systemshardening.com/articles/linux/luks-tpm2-sealing/
15. Endpoint encryption guide (escrow before enforcing): https://cybersecuritynews.com/best-endpoint-encryption-software/amp/
16. QEMU record/replay: https://www.qemu.org/docs/master/system/replay.html
17. QEMU TCG icount: https://www.qemu.org/docs/master/devel/tcg-icount.html
18. Linux lockup watchdogs: https://www.kernel.org/doc/html/latest/admin-guide/lockup-watchdogs.html
19. KCSAN: https://docs.kernel.org/dev-tools/kcsan.html
