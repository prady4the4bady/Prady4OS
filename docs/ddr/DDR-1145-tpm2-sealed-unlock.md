# DDR-1145 — TPM 2.0: transport, measured kernel, sealed volume key

**Status: DESIGN. Committed before code (NON-NEGOTIABLE 5).** Part of the track
DDR-1143 opens. A subsystem of its own, so it has its own DDR, as the operator
asked.

## §1 What exists

Measured: **nothing.** `grep -rniE tpm kernel boot` is empty (and checklist #69
records the same). **swtpm is not installed in this container, but it IS in the
Ubuntu 24.04 archive** (`apt-cache policy swtpm` gives candidate
`0.7.3-0ubuntu5.24.04.1`). So CI can get it through `tools/ci/apt_prepare.sh`,
exactly as it gets `ovmf`. QEMU 8.2 attaches it as `-tpmdev emulator` with
`-device tpm-tis` or `tpm-crb`.

**The first measurement, before any other code:** whether Ubuntu's
`OVMF_CODE_4M.fd` is built with TCG2 support. It must publish
`EFI_TCG2_PROTOCOL` and extend PCR0/2/4/7 when a TPM is attached. If it does
not, the measured-kernel half (§3) cannot be exercised in CI with the
distribution OVMF. The track then **stops and reports** rather than inventing a
substitute.

### §1.1 Measured 2026-09-26: Ubuntu's OVMF DOES measure into a TPM

Measured after the operator approved the track (PR #17 comment 5841525203),
which asked for this before anything else on the TPM path.
`tools/ci/tpm_ovmf_probe.sh` boots `OVMF_CODE_4M.fd` (package `ovmf
2024.02-2ubuntu0.9`, QEMU 8.2.2, swtpm 0.7.3 from the archive) with no disk and
no OS, once per interface, and decodes every command the firmware sends:

| Interface | Commands | PCR_Extend by PCR |
|---|---|---|
| `tpm-crb` | 133 (Startup, SelfTest, 70 GetCapability, 30 PCR_Read, 29 PCR_Extend) | 0:4, 1:12, 2:2, 3:1, **4:2**, 5:1, 6:1, 7:6 |
| `tpm-tis` | 134 (same, plus one locality command) | identical |

- **PCR 4 is extended.** That measurement is made in the DXE phase, which is
  where `Tcg2Dxe` runs, and `Tcg2Dxe` is the driver that installs
  `EFI_TCG2_PROTOCOL`. So the firmware-side TCG2 stack is present on **both**
  interfaces, and the track does **not** stop.
- **PCR 9 is never touched**, so the kernel measurement in §3 has it to itself.
- **Inference, not measurement:** that our loader can `LocateProtocol` the
  protocol. That is measured directly by the loader's first call when §3 is
  built. If it fails, §3 stops and reports.
- Two command codes are left undecoded in the histogram (`0x181` before
  Startup, `0x129` once). They are printed as hex, not guessed.
- Cost: ~80 s for both interfaces (the firmware idles to its PXE timeout).

## §2 Transport and command subset

- **Discovery:** the ACPI `TPM2` table's start method:
  - FIFO/TIS at `0xFED40000` (locality 0);
  - CRB (the interface of firmware TPMs: Intel PTT, AMD fTPM).
  - **Both are required.** A TIS-only driver would miss most laptops sold
    today. QEMU emulates both, so both are gateable.
- **Commands** (marshalled big-endian; the TPM does all asymmetric crypto
  internally, so **no new primitive is needed for sealing**):
  - `GetCapability`, `PCR_Read`;
  - `CreatePrimary`: the storage root key, a TCG-standard template, recreated
    each boot, so no persistent handle is used;
  - `Create`: a KEYEDHASH sealed-data object holding the 32-byte keyslot-T
    secret (DDR-1144 §3), with `authPolicy` = the policy digest;
  - `Load`, `StartAuthSession` (policy and **trial** sessions),
    `PolicyPCR`, `PolicyAuthValue` (the PIN option, §5), `Unseal`,
    `FlushContext`.
- **The policy digest is computed by the TPM in a trial session, not
  hand-rolled.** Rolling the `PolicyPCR` hash chain by hand is the classic
  source of objects that seal fine and never unseal.

## §3 Measured kernel, and why it is mandatory rather than optional

UEFI firmware measures what it runs:

- PCR0: firmware code;
- PCR4: the boot application, i.e. **our 6 KiB loader**;
- PCR7: the Secure Boot state.

It does **not** measure `KERNEL.BIN`, which our loader reads from the
**unencrypted** ESP. A policy over only the firmware PCRs would unseal the
volume key into **any** kernel placed on the ESP, including one written to
print the key. The TPM layer would then be decorative, which the instruction
forbids.

So the UEFI loader calls `EFI_TCG2_PROTOCOL.HashLogExtendEvent` over the
`KERNEL.BIN` bytes into **PCR 9** before jumping. It is the same byte range it
hands the kernel, and the same range DDR-1143 §2's pristine copy uses.

- **Sealing policy:** PCRs **{0, 4, 7, 9}**.
- **Consequence:** a firmware update, a loader rebuild (and the loader hash is
  already known not to be reproducible: lld-link's `TimeDateStamp`, DDR-1142
  §5) or a kernel update changes a PCR. The unseal then fails and the
  **passphrase path takes over**. After a successful passphrase unlock the OS
  re-seals to the new values (DDR-1144 §5.6). That is the designed update
  story, not a failure mode.

## §4 BIOS boots, decision D6

A legacy BIOS path would need stage1 and stage2 to measure through TCG
`INT 1Ah`. That is a different, older interface, SeaBIOS's support varies, and
stage2 has 8 KiB of 16-bit assembly budget. **BIOS-booted installs get
passphrase and recovery only.** Keyslot T is never created or used on a BIOS
boot, and the boot log says so in one line. Recommended: accept.

## §5 TPM-only auto-unlock vs TPM+PIN, decision D4, stated plainly

This OS has **no user accounts and no login** (DDR-1143 §4, decision #25). With
TPM-only unlock, a stolen machine that is powered off boots, unseals, and lands
on a usable desktop. The encryption then protects against:

- the **disk being removed** and read elsewhere;
- the **boot chain being altered** (the PCR mismatch sends it to the
  passphrase).

It does **not** protect against **someone who simply turns the machine on.**
That is how TPM-only BitLocker behaves too, but there a Windows login sits
behind it, and here nothing does.

**TPM+PIN** adds `PolicyAuthValue`, with the PIN as the sealed object's
`authValue`:

- the PIN is checked **by the TPM**;
- guesses are limited by the TPM's own **dictionary-attack lockout**, so a
  6-digit PIN is strong in a way a 6-digit passphrase against an offline KDF is
  not;
- the stolen-and-powered-on case is closed.

**Recommendation: TPM+PIN.** Or TPM-only with the limitation written into the
release notes. It is the operator's call (DDR-1143 §8 D4). The passphrase and
recovery slots exist in both options.

## §6 What swtpm can verify here, and what needs physical hardware

**Verifiable in CI with swtpm + OVMF (if §1's measurement passes):**

- TIS and CRB transport;
- command marshalling;
- seal and unseal;
- PCR-policy enforcement;
- fallback when the kernel changes (PCR 9 moves), when the TPM is cleared (a
  new swtpm state directory), and when there is no TPM;
- re-seal after fallback;
- PIN checking and lockout **logic**.

**NOT verifiable without physical hardware, and not claimed:**

1. **Discrete-TPM bus exposure.** On a dTPM the unsealed secret crosses the
   LPC/SPI bus in the clear, and published attacks sniff exactly this against
   BitLocker. The mitigation is TPM2 **parameter encryption in a salted
   session**. The salt must be encrypted to a TPM key with **RSA-OAEP or ECDH
   on P-256, neither of which is in this tree** (X25519 is not a TPM curve).
   So it needs a new primitive, and it is **named, not built**. Firmware TPMs
   (on-CPU) do not have this bus.
2. **Vendor quirks** of real dTPMs and fTPMs: command timing, CRB
   idle/ready semantics, locality handling on specific chipsets.
3. **Real firmware PCR values and event logs.** OVMF's measurements are a
   proxy.
4. **Lockout timing** on real chips.
5. **TPM 1.2 machines.** Unsupported. TPM 2.0 only, stated in the release
   notes.
6. **Evil-maid passphrase phishing.** A replaced kernel fails the unseal, and
   *its own* prompt could then capture the passphrase. Closing that needs a
   TPM-sealed "the machine is genuine" secret shown to the user before they
   type (for example TPM-sealed text). **Named, not built.**

## §7 Gate: `smoke-tpm` (swtpm, OVMF), vacuity checked

| Arm | Assertion | Mutant that must fail it, and only it |
|---|---|---|
| A | An installed disk with swtpm boots **with no passphrase injected** and reads back the DDR-1143 nonce marker. | M1: T slot never written |
| K | The same disk with `KERNEL.BIN` replaced by a **different valid kernel** (one byte changed in a padding region, confirmed by hash) **does not auto-unlock**: the passphrase prompt appears. The prompt text is asserted, and a timeout is not accepted as the pass. | **M2: policy omits PCR 9.** It auto-unlocks the modified kernel. **This is the load-bearing mutant**, because it is exactly the decorative-TPM failure. |
| X | A fresh swtpm state (the TPM has been cleared) sends boot to the prompt, and the passphrase unlocks it. | M3: fallback path returns an error instead of prompting |
| E | After X, a re-seal makes the next boot automatic again. | M4: re-seal not persisted |
| B | A BIOS boot of the same disk prints `[tpm] bios path: passphrase only` and never touches the TPM. | — |

A vacuous version of arm A would be "no prompt appeared". A kernel that skips
unlocking and roots at a RAM disk also shows no prompt. The nonce read-back
closes that.

## §8 Not claimed

- No implementation exists.
- Nothing here protects against a dTPM bus sniffer (§6.1) or passphrase
  phishing (§6.6).
- The OVMF TCG2 capability is **unmeasured** until §1 is run.
