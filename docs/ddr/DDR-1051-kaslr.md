# DDR-1051 — KASLR ASSESSED and NOT BUILT: buildable, but wrong to build now

**Status:** ASSESSED, deferred. This one is **not** blocked by an absent
subsystem the way DDR-1038 (`SYS_FUTEX`) and DDR-1050 (I/O APIC stage D) were.
It is a **judgment call about sequencing**, and it is recorded as one rather than
dressed up as an impossibility.
**Group A row:** *"KASLR — after W^X is CI-green — gate `smoke-kaslr`."* W^X **is**
CI-green (`smoke-wxkernel`, DDR-1046), so the stated precondition is met.

---

## 1. What KASLR would cost here, measured

### 1.1 The kernel cannot slide as linked

`KCFLAGS` is `-fno-pic -fno-pie -mcmodel=kernel`, and `kernel.ld` pins
`KERNEL_VBASE = 0xFFFFFFFF80000000` with `. = KERNEL_VBASE`. `mcmodel=kernel`
emits **absolute, sign-extended 32-bit** references into `.text`, so a slide is
not a matter of changing one constant.

Measured: `llvm-readelf -S build/kernel.elf | grep -c rela` returns **0** — the
linked image carries **no relocation sections at all**. A virtual slide therefore
requires *both* a relink (`--emit-relocs`, or a PIE/`-mcmodel=large` build) *and*
a boot-time relocation applier that does not exist.

Entropy budget is also bounded by the memory model: `mcmodel=kernel` requires the
kernel to live in the top 2 GB, so at 2 MiB granularity the slide has ~1024 slots
(≈10 bits) *before* subtracting the image size — modest, and worth stating rather
than implying "randomised" means "unguessable".

### 1.2 It lands in TWO boot paths, not one

§INV.13: PT_HI is implemented **twice** — `boot/stage2/stage2.asm` (BIOS) and
`boot/uefi/loader.c` (UEFI). Both hardcode `KERNEL_PHYS`/`KERNEL_VIRT` and both
build the higher-half mapping. KASLR must land in both **in the same commit**, or
one boot path breaks — and `smoke-iso-x86` has BIOS *and* UEFI arms, so a
half-done change fails the release gate rather than passing quietly.

### 1.3 Entropy at that point in boot

`RDSEED` exists in the tree but only as a driver-time helper
(`virtio_rng.c:37-47`, CPUID-guarded). At stage-2 / early-kernel there is no RNG
subsystem, so KASLR needs its own CPUID-guarded `RDSEED`/`RDRAND` path with a
stated fallback. Buildable; simply not free.

## 2. THE REASON IT IS DEFERRED, AND IT IS NOT DIFFICULTY

**KASLR would degrade the one diagnostic discipline every open defect in this
project currently depends on.**

§INV.18 and DDR-1019 both mandate the same rule before reading any `[apfreeze]`:
**resolve its RIP against its own binary.** That is not advice — it is how
DDR-1019 established that the shard-9 freeze was the panic-arbitration loser's
halt loop at `idt.c:697`, by disassembling the exact CI binary rebuilt
bit-for-bit. `[apfreeze]` alone has **at least three distinct producers**;
telling them apart is done entirely by RIP.

Four defects are open right now and every one of them is diagnosed that way:

| open item | how it is read |
|---|---|
| OPEN-1 | a CI-only stop point; candidate producers separated by RIP |
| OPEN-2 | three `[apfreeze]` producers, distinguished **only** by RIP |
| OPEN-12 | ring-0 exception; identity unproven precisely because its RIP was lost |
| OPEN-13 | `freed_by=`/`now_by=` return addresses, resolved against the binary |

With a random slide, **every future CI artefact's RIP requires the slide value to
interpret.** The kernel would have to print it, and a kernel that prints its own
slide on every boot has given it back to the local attacker the mitigation
targets. Linux resolves this by printing it only under a debug flag — which here
would mean CI runs with the flag on (so the artefacts stay readable) and the
shipped ISO with it off, i.e. **CI would stop testing the configuration that
ships**, the exact vacuity DDR-1040 was written to avoid.

### 2.1 And the marginal security value is low *here*

KASLR raises the cost of turning an information leak into a working ROP chain.
This kernel already has, all CI-green: **W^X including the identity alias**
(DDR-1046 — the write-via-alias hole is closed), **SMEP** (DDR-1040), and **SMAP**
(DDR-1041). Those remove the primitives KASLR only makes harder to aim. The ring-3
population is first-party probes plus PRISM; there is no untrusted local user.

So the trade is: a bounded (~10-bit) mitigation, layered on top of three
enforcement mechanisms that are already done, purchased with a relink, a new
relocation applier, changes to both boot paths, and **the loss of RIP-resolvable
crash artefacts while four defects are open** — days from a release whose
`v1.0.0` tag is already held.

## 3. The cheaper variant, and why it is not obviously worth it either

**Physical-only KASLR** — randomise `KERNEL_PHYS`, keep `KERNEL_VIRT` fixed —
needs **no relocations at all**, because every symbol keeps its virtual address.
It also leaves RIP→symbol resolution completely intact, so §2's objection does
not apply to it.

It is genuinely cheaper. It is recorded here as the buildable option. But note
what it defends: an attacker with a *physical*-address write primitive — and
DDR-1046 has just made the identity alias **RO+NX**, which removes that primitive.
It still touches both boot paths and the DDR-1046 alias audit (which currently
expects the kernel at PD entry 2), so it is not free either.

## 4. Decision

**Deferred.** Not because it cannot be built — it can — but because the
sequencing is wrong: it degrades RIP-based crash diagnosis while four defects
depend on exactly that, and it layers a ~10-bit mitigation on top of W^X, SMEP
and SMAP, which are already shipped and green.

**Revisit after** OPEN-1/OPEN-2/OPEN-12/OPEN-13 are closed, when losing
straightforward RIP resolution costs less. At that point the order is: relink
with `--emit-relocs`, write the relocation applier, land it in **both** boot
paths in one commit, add a CPUID-guarded `RDSEED` entropy path, and gate it with
a marker that proves the slide was actually applied — a gate asserting only "the
kernel booted" passes with a slide of zero, which is DDR-1040's vacuity trap.

**Not claimed:** that KASLR is unimportant, or that this kernel is hardened
without it. Only that it is the wrong item to land this week.

## 5. Files

None — assessment only.
