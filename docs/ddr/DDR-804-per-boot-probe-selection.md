# DDR-804 — per-boot probe selection, so a probe can exist in exactly one gate

**Status:** accepted; implemented in this slice.
**Date:** 2026-07-30
**Closes:** OPEN-7.
**Unblocks:** the DDR-802 gate, and every future gate over global kernel state.

## The problem, stated exactly

Every probe in `kmain` is spawned with `sched_unblock` and runs **concurrently**
with every other probe. `user_boot_from_sfs` writes the embedded ELF into SFS and
loads it unconditionally, so there is no existing way for a probe to be present
in one gate and absent in the rest. The `-drive`/`-device` knobs
(`QEMU_SFSROOT`, `QEMU_SFS2`, …) change *storage topology*, not which probes run.

For probes that only read state this is harmless, which is why it has never
mattered. It stops being harmless the moment a probe **mutates global kernel
state**. DDR-802's privacy mode is the first: switching it on for the two
syscalls the probe needs also refuses the concurrent connects in `capnettest`,
`sovegresstest` and `egressaudittest`, failing *their* gates.

The window is small. That is precisely the problem — small enough to pass
locally every time and to hit eventually in CI, which is the exact signature that
cost four refuted hypotheses on BUG-1 and one wasted CI cycle on `rtcmonotest`.
A gate that destabilises its neighbours is worse than no gate.

This will recur for every future global-state control (a global rate limit, a
kill switch, a scheduler-lane override). It needs a general mechanism, not a
second special case.

## Rejected: rebuild the kernel per gate

A compile-time `-DPROBE_PRIVNET` is the obvious answer and is wrong here. Gates
depend on `$(IMG)`, which is built **once** and reused by all of them; making
probe selection a compile-time property would force a distinct kernel image per
gate. That multiplies build time across ~113 gates and, worse, destroys artefact
identity — the gates would no longer be testing the same binary, so a green suite
would stop meaning "this image is good". DDR-791's invalidated A/B is the
standing reminder of what happens when the artefact under test is not the
artefact you think it is.

Selection must therefore be a **boot-time input to one fixed image**.

## Rejected: another block device

The `QEMU_SFSROOT` precedent (kernel infers intent by peeking the highest block
index) works, but it is not general: it consumes one of `VBLK_MAX = 4` slots,
conflates "which test am I" with "what storage exists", and cannot express more
than a handful of states before the encoding collapses. Extending it would make
storage topology the carrier for unrelated configuration.

## Decision: QEMU `fw_cfg`, the channel built for this

QEMU's firmware-configuration device is the host→guest configuration channel,
and this is exactly its purpose. Per QEMU's `fw_cfg` interface on x86:

* selector register `0x510`, written 16-bit (`outw` — already in `kernel/io.h`);
* data register `0x511`, read 8-bit (`inb` — likewise);
* key `0x0000` returns the four-byte signature `QEMU`;
* key `0x0019` is the file directory: a big-endian `u32` count followed by
  entries of `{u32 size, u16 select, u16 reserved, char name[56]}`, also
  big-endian.

The harness passes `-fw_cfg name=opt/org.pradyos/probes,string=<csv>`; the kernel
walks the directory for that name and reads the comma-separated list.

Why this and not SMBIOS OEM strings (the other general option): SMBIOS needs a
table scan and an entry-point parser, whereas fw_cfg needs two port accesses the
tree already has. **No new I/O primitives and no new hardware assumptions** —
`inb` and `outw` exist at `kernel/io.h:9` and `:15`.

The claimed register and directory layout above is taken from QEMU's documented
`fw_cfg` interface. It is **verified empirically by the gate**, not asserted: the
signature check below fails closed, and `smoke-privacy-netfilter` cannot pass
unless the string actually arrived. If the layout were wrong, the probe would
simply never be selected and the gate would fail — it cannot silently "pass" on
a bad read.

## Fail-closed, and why that direction

The signature at key `0x0000` is checked first. Anything other than `QEMU` —
device absent, different machine type, a future non-QEMU target — yields **an
empty probe set**, and every opt-in probe stays off.

The default direction is the safety property. An opt-in probe that fails to be
selected makes *its own* gate fail, which is loud, local, and points straight at
the cause. A probe that got selected when it should not have been would perturb
unrelated gates, which is silent, remote, and is the whole defect this DDR
exists to remove. Failing toward "off" makes the bad outcome the detectable one.

## Scope: new probes only

`probe_enabled()` is consulted **only** by probes introduced as opt-in. Every
probe that is unconditional today stays unconditional. This is deliberate: the
blast radius of this DDR is exactly the probes that did not exist before it, so
it cannot regress the 113 gates currently green. Migrating existing probes onto
the mechanism, if ever wanted, is a separate change with its own A/B.

## Bounds (S2)

Every loop is bounded, and the bounds are checked before use rather than trusted
from guest-visible data:

* directory entries scanned: `FWCFG_MAX_ENTRIES = 64`, and the count read from
  the device is clamped to it — a hostile or corrupt count cannot drive the loop;
* probe-list bytes read: `FWCFG_PROBES_MAX = 256`, and the entry's declared size
  is clamped to it before any read;
* name comparison is length-bounded against the fixed 56-byte field, which is
  explicitly **not** assumed NUL-terminated.

No allocation: the list lands in a fixed static buffer, read once at boot into
kernel memory that ring 3 never sees.

## Gate

This DDR ships the mechanism; `smoke-privacy-netfilter` (DDR-802) is its first
consumer and its real test. The mechanism is proven by that gate's three arms —
if selection did not work, the privacy probe would never run and the gate would
fail on a missing sentinel.

A dedicated `smoke-fwcfg` gate is deliberately **not** added: it would assert
that a string survives a port read, which the DDR-802 gate already demonstrates
end to end. A second gate testing the transport in isolation would add CI time
(and, per DDR-803, these windows are not free) for no additional discrimination.
