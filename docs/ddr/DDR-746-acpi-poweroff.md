# DDR-746 — ACPI S5 poweroff: `SYS_POWEROFF` + clean shutdown

**Status:** proposed (pre-code)
**Layer:** 3 (ACPI/power) + syscall + user (compositor). First power-management slice.

## Problem

"ACPI Power Management (FADT/power)" is 🔴 NOT BUILT. The ACPI table parser is
complete (`acpi_find_table`, RSDP/RSDT/XSDT), but nothing parses the **FADT
(`FACP`)** or performs a controlled machine shutdown. A sovereign OS must be able
to power itself down; today the only "exit" is the QEMU timeout killing the VM.

## Decision

Add ACPI S5 (soft-off) poweroff, wired to a capability-gated syscall.

**Kernel (`kernel/acpi/`).** `acpi_power_init()` (called from kmain after
`acpi_init()`):
- `acpi_find_table("FACP")` → FADT. Read, by ACPI-spec field offset (ACPI 6.x
  §5.2.9, Table "Fixed ACPI Description Table"): `PM1a_CNT_BLK` (off 64, u32),
  `PM1b_CNT_BLK` (off 66, u32), `DSDT` (off 40, u32).
- Locate the `\_S5_` object in the DSDT AML and extract `SLP_TYPa`/`SLP_TYPb`
  (the values written to the PM1x_CNT `SLP_TYP` field, bits 10–12). This uses the
  standard minimal `_S5_` scan (find the `_S5_` NameOp + following `PackageOp`,
  read the first one/two integer elements) — not a full AML interpreter, which is
  out of scope. Cited: ACPI §7.4.2 (`_S5`) + §4.8.3.2.1 (PM1 control, `SLP_EN`
  = bit 13, `SLP_TYP` = bits 10–12).

`acpi_poweroff()` (no return): prints the `PRADYOS_POWEROFF` sentinel, then
`outw(PM1a_CNT, (SLP_TYPa<<10)|SLP_EN)` (and PM1b if present). On real hardware /
QEMU this transitions to S5 and the machine powers off; if it returns, it `hlt`s
forever. `outw`/`inw` are added to `kernel/io.h` (only `outb`/`inb` exist today).

**Syscall.** `SYS_POWEROFF` (NSI 69), **CAP_SOVEREIGN-gated** (mirrors
`SYS_SET_MODE`: `if (!current_thread->is_sovereign) return -EPERM;`). On authority
it calls `acpi_poweroff()` and does not return. A non-sovereign caller gets
`-EPERM` and survives — no self-escalation (ADR-026 §D6).

**User.** The compositor (spawned CAP_SOVEREIGN) gains a `q` key → `SYS_POWEROFF`,
alongside its existing `m`/`s` mode keys.

## Gate — `smoke-poweroff` (new; 84 → 85)

The poweroff must NOT fire in the shared boot (it would kill every other gate's
VM). Isolation: the trigger is a `q` keypress, and only `smoke-poweroff` sends
it. Modeled on `smoke-compositor` (GPU boot + QMP `sendkey` via
`input_inject.sh`): boot with `-device virtio-gpu-pci` + a monitor socket, wait
for `PRADYOS_COMPOSITOR_OK`, then `sendkey q`. QEMU has no `-no-shutdown`, so the
S5 write makes QEMU exit. Assert `PRADYOS_POWEROFF` appears in the serial log
(printed immediately before the PM1a write) and no kernel panic. Every other
gate omits the `q` key, so their boots are unaffected.

## Non-goals

- No full AML interpreter — only the minimal `_S5_` package scan. If a future
  board hides `_S5_` behind AML control flow, this returns "no S5" and skips.
- No S1–S4 sleep states, no wakeup, no ACPI reset register (a sibling later
  slice), no per-device power (D-states).
- No graceful userspace teardown / sync on shutdown — the fs volumes are already
  committed per-write (SFS journal / FAT). A shutdown-time flush is future.
