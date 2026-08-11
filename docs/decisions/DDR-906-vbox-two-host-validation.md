= DDR-906 — item 49 SHIPPED: VirtualBox validation, and the two-host split

**Status:** Accepted. Item 49 **CLOSED**, proven by a real run.
**Date:** 2026-08-11

## The split, stated plainly

Every other gate in this project runs QEMU inside WSL. This one cannot, and the
reason is structural rather than incidental:

```
WSL      builds the ISO    make iso
Windows  boots the ISO     tools/vbox/run_vbox.ps1
```

VirtualBox is a type-2 hypervisor and needs VT-x directly. WSL2 is itself a
Hyper-V guest, and nested VT-x is not exposed to it. No amount of configuration
inside WSL produces a working VirtualBox there.

This is recorded as an architectural constraint, not hidden as a workaround. The
practical consequence is that **item 49 cannot join the CI shard matrix** — the
CI runners have no VirtualBox and no Windows host. It is an owner-run check, and
`tools/ci/gate_shards.txt` is untouched.

## What it proves that the QEMU gates do not

VirtualBox is neither SeaBIOS nor OVMF. A pass here is independent evidence that
the boot chain does not depend on one firmware vendor's behaviour.

That is not hypothetical: DDR-905/907 is precisely a case where the boot chain
worked on one firmware and failed on another, and where a *wrong* conclusion was
drawn from a single firmware's register value. A second-source firmware check is
the cheapest defence against that whole class of error.

## The run

```
pwsh -File tools/vbox/run_vbox.ps1 -Iso <path> -Firmware efi
```

```
[vbox] pradyos-gate-efi-01d89739 firmware=efi
[uefi] PRADYOS loader
[uefi] handoff
NEXUS: entered kmain (64-bit long mode, ring 0)
NEXUS: E820 map, entries=0x00000000000000
0A
...
NEXUS KERNEL OK
...
NEXUS: starting scheduler
[vbox] PASS — 'NEXUS KERNEL OK' on efi
```

Exit code 0. Not merely the sentinel: the kernel ran through PMM, heap, VMM, COW
fork, the full 11/11 capability suite, uaccess, ACPI, APIC, SMP bring-up and RNG
to the scheduler, on a hypervisor it had never been booted on before. VirtualBox
reported 10 E820 entries and the kernel consumed them correctly.

## Design notes worth keeping

- **The ISO is copied to a local Windows path first.** VirtualBox attaches media
  by path, and `\\wsl$\...` is served by a 9p redirector that VBoxSVC cannot
  reliably open. Copying costs a second and removes a class of "works in my
  shell, fails in the service" failures.
- **`VBoxManage` exit codes are checked on every call.** It reports failure via
  exit status, not stderr text. Without the check a misconfigured VM boots
  nothing and the sentinel timeout blames the kernel for a harness bug — the
  same "check that absorbs invalid input" shape this project keeps finding.
- **The VM is unregistered in a `finally` block**, so a failed run does not leak
  VMs into the user's VirtualBox registry.
- `-Firmware bios` is implemented and will be the natural regression check once
  item 48's BIOS arm works. It is **not claimed as passing today** — the ISO's
  BIOS arm does not boot anywhere yet (DDR-907).

## Scope

Proven: **EFI arm on VirtualBox 7.2.8r173730.** Not proven: the BIOS arm, which
is blocked upstream by item 48 and is not item 49's defect.
