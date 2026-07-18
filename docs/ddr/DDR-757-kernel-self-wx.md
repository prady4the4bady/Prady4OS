# DDR-757 — kernel-self W^X: kernel text RX, kernel data NX

**Status:** proposed (pre-code)
**Layer:** mm/boot. M1 kernel hardening 1/3. Closes the ADR-021-deferred item
("the bootloader maps the kernel image RWX; only user W^X is enforced today").

## Problem

Stage2's PT_HI maps the whole kernel image (`KERNEL_VBASE` → phys `0x400000`,
4 KiB pages, 2 MiB span) present+RW with no NX — kernel `.text` is writable and
kernel `.data`/`.bss`/`.rodata` are executable. The low identity map additionally
aliases the image at `0x400000` via a 2 MiB RWX PDE. User W^X (ADR-021) is
enforced; the kernel's own image is not.

## Decision

**1. Page-align the section boundaries (`kernel/kernel.ld`).** `.rodata` and
`.data` become `ALIGN(4096)` and the script exports `__text_end` /
`__rodata_end` (page-aligned). Without this a page straddles text+rodata and no
per-page policy is expressible. Costs ≤ 2 pages of image (kernel ~590 KiB of the
2 MiB span).

**2. `vmm_protect_kernel()` (`kernel/mm/vmm.c`),** called from kmain right after
`vmm_init()` (NXE armed, master CR3 recorded), before any user process:
- Walk the master tables `PML4[511] → PDPT[510] → PD[0] → PT` (the PT_HI page)
  and set, per PTE by VA:
  - `[KERNEL_VBASE, __text_end)` → clear `RW` (text RX; NX stays clear).
  - `[__text_end, __rodata_end)` → clear `RW`, set NX (rodata R, no-exec).
  - `[__rodata_end, span end)` → keep `RW`, set NX (data/BSS/spare RW, no-exec).
- Set NX on the **identity-map PDE for `0x400000..0x600000`** (`PML4[0] →
  PDPT[0] → PD[2]`, a 2 MiB page) — kills execute-via-alias. The PDE stays RW:
  the PMM pool starts at 16 MiB, so nothing else lives in that range, but leaving
  it writable avoids breaking any residual boot-era writer; the write-via-alias
  residue is documented below.
- NX bits are set only when `g_nx_ok` (same gating as user W^X); RW-clearing on
  text is unconditional.
- Reload CR3 (full non-global flush), then **audit**: re-walk the PT and verify
  no text PTE has RW and (when NX) every non-text PTE has NX — print
  `[wx] kernel W^X OK` / `kernel W^X FAIL`.

Per-process ASes share the kernel top-level entries (vmm.c:185), so this single
pass hardens the kernel mapping in **every** address space, present and future.

**3. AP NXE ordering fix (`arch/x86_64/ap_boot.asm` + `kernel/apic/smp.c`).**
Found while threat-modeling this change: the AP trampoline sets only `EFER.LME`,
enables paging, and jumps into higher-half C — which touches kernel `.data`/
`.bss` *before* `vmm_enable_nxe_ap()` runs. With NX now set on kernel-data PTEs,
every AP would take a reserved-bit #PF in that window (exactly the
`ap-percpu-machine-state` failure mode). Root fix: the mailbox gains an
`efer_or` u32 (BSP writes `LME | (g_nx_ok ? NXE : 0)`), and the trampoline ORs
that instead of the hardcoded `0x100` — NXE is armed *before* paging, and a
non-NX CPU still gets plain LME. The later `vmm_enable_nxe_ap()` call stays
(harmless idempotent re-arm).

## Residual exposure (documented, follow-on)

Kernel text remains *writable* through the identity alias (`0x400000..` RW+NX).
Removing that requires either 4 KiB-splitting the identity PDE or dropping the
alias after boot — deferred (nothing legitimate writes it; an attacker needs an
arbitrary-write primitive first, at which point they can also edit PTEs). The
gate proves the higher-half image obeys W^X; the alias is the recorded residue.

## Gate — `smoke-wxkernel` (new; 92 → 93)

The audit sentinel `[wx] kernel W^X OK` (EXTRA_SENTINEL) with `kernel W^X FAIL`
forbidden. A CPL-0 negative test (write to text → #PF) is deliberately not a
boot gate: kernel faults panic by design (ADR-021 §3), so the positive PTE audit
is the deterministic witness; every existing gate doubles as no-regression proof
(all kernel + user code paths run against the hardened tables — SMP gates cover
the AP NXE ordering).

## Non-goals

- No identity-alias write protection (above), no KASLR, no kernel guard pages.
- No per-AP audit (EFER is per-CPU but PTEs are shared; the SMP gates exercise
  AP execution against the hardened tables).
- No module/driver split — the kernel is one image with one text range.
