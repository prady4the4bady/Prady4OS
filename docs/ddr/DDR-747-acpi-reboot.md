# DDR-747 — ACPI reboot: `SYS_REBOOT` + FADT reset register

**Status:** proposed (pre-code)
**Layer:** 3 (ACPI/power) + syscall + user (compositor). Sibling of DDR-746.

## Problem

DDR-746 gave the sovereign OS a controlled power-off (ACPI S5). The matching
capability — a controlled **reboot** — is still missing (named as a sibling in
DDR-746's non-goals). A machine you can only turn off is half a power story.

## Decision

Add `acpi_reboot()` + a CAP_SOVEREIGN `SYS_REBOOT`, reusing the DDR-746 FADT
parse.

**Kernel (`kernel/acpi/`).** Extend `acpi_power_init()` to also read the FADT
reset support (ACPI 6.x §5.2.9): `Flags` (off 112, u32; `RESET_REG_SUPPORTED` =
bit 10), the `RESET_REG` Generic Address Structure (off 116, 12 bytes: space id,
width, offset, access size, then a u64 address), and `RESET_VALUE` (off 128, u8).
When `RESET_REG_SUPPORTED` is set and the register is System-I/O
(`address_space_id == 1`), record the port + value.

`acpi_reboot()` (no return): prints `PRADYOS_REBOOT`, then, in order until the
machine resets:
1. the FADT reset register write (if parsed), then
2. the PCI reset-control port `0xCF9 <- 0x0E` (SYS_RST|RST_CPU|FULL_RST), then
3. the 8042 keyboard-controller pulse `0x64 <- 0xFE` (asserts the CPU reset line).
Steps 2–3 are the standard PC fallbacks that always work under QEMU, so reboot is
robust even when the FADT omits a usable reset register. If all fail it `hlt`s
forever.

**Syscall.** `SYS_REBOOT` (NSI 70), CAP_SOVEREIGN-gated exactly like
`SYS_POWEROFF` (`-EPERM` for non-sovereign; no return on success — reset is always
attempted, so no `-ENODEV`).

**User.** The compositor gains a `b` key → `SYS_REBOOT` (alongside `p` = poweroff,
`q` = exit).

## Gate — `smoke-reboot` (new; 85 → 86)

Same isolation as `smoke-poweroff` (only this gate sends `b`): GPU boot +
`input_inject.sh` waits for `PRADYOS_COMPOSITOR_OK` then `sendkey b`. The gate's
QEMU runs with `-no-reboot`, so a CPU reset makes QEMU **exit** instead of
looping. Assert the pre-reset `PRADYOS_REBOOT` sentinel (and the compositor's
`PRADYOS_COMPOSITOR_REBOOT` key-path marker) appear, and no panic.

## Non-goals

- No warm-vs-cold distinction, no reboot-to-firmware, no kexec.
- No S1–S4 sleep/wake; no per-device D-states.
- Reset method selection is best-effort (try ACPI reg, then 0xCF9, then 8042);
  no ordering policy beyond "first one that resets wins".
