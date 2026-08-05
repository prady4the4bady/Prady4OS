# DDR-841 — reproducible build env, VirtualBox runner, x86_64 chipset matrix

**Status:** accepted
**Date:** 2026-08-06
**Governs:** `Dockerfile`, `tools/vbox_runner/`, `make smoke-chipset`
**Covers:** Group 1 items 2, 4, 5 (item 3 assessment is separate, below)

## Item 5 — chipset matrix is the one that finds bugs, so it leads

Every gate in the tree boots `-M q35`. That is one chipset, one PCI topology, one
interrupt-routing model. A kernel that boots only on q35 is a kernel with an
undiscovered dependency on q35, and the release claim is "boots on x86_64", not
"boots on the machine type our gates happen to use".

`smoke-chipset` boots the SAME image across the variants QEMU can actually give
us on an x86_64 host:

| variant | what it changes |
|---|---|
| `-M q35 -cpu qemu64` | the existing baseline (PCIe, ICH9) |
| `-M pc -cpu qemu64` | i440FX: legacy PCI, different IRQ routing |
| `-M q35 -cpu Nehalem` | Intel feature set |
| `-M q35 -cpu Opteron_G5` | **AMD** feature set — different CPUID, different MSR availability |

Each arm must reach the same boot sentinel. The AMD arm is the point: every
CPU-feature assumption in the tree was written and tested on an Intel-flavoured
`qemu64`, and a missing CPUID guard shows up there and nowhere else.

**Scope limit, per the brief:** x86_64 only. ARM64/RISC-V variants stay deferred
with the arch ports.

## Item 2 — Docker reproducible build

`Dockerfile` pins the exact toolchain the project builds with (clang, lld, nasm,
QEMU, dosfstools, mtools) on a pinned Ubuntu 24.04 base, and `make docker-build`
builds the image inside it.

**What this does and does not claim.** It makes the build environment
*reproducible* — the same container yields the same toolchain versions. It does
NOT make the build bit-for-bit reproducible; that needs `SOURCE_DATE_EPOCH`
handling and a deterministic link order audit, which is a separate piece of work
and is not claimed here. Saying "reproducible build" when only the environment is
pinned is the kind of overclaim this project keeps correcting.

## Item 4 — VirtualBox runner

`tools/vbox_runner/run_vbox.sh` converts `build/pradyos.img` to VDI, creates a
headless VM, boots it with a serial file, and greps for the sentinel — the same
contract as `boot_test.sh`, different hypervisor. Required by Delivery D.2.

**It cannot run in CI**, and that is stated rather than hidden: GitHub's runners
have no VirtualBox, and nesting VirtualBox inside a VM is not supported. The
script therefore **detects a missing `VBoxManage` and exits with a distinct code
(77) that means "not available here", not 0.** A runner that silently "passes"
when the hypervisor is absent would be the eighth instance of this project's
recurring defect — a check that reports success without checking anything.

CI validates what it honestly can: the script is syntax-checked
(`bash -n`) by `make ci-vbox-check`, and the operator runs the real boot locally
for D.2 sign-off.

## The rule this earns

**A test rig that cannot run in CI must fail loudly where it cannot run, never
quietly succeed.** Exit 77 forces the caller to decide; exit 0 would let a green
pipeline imply a VirtualBox boot that never happened.
