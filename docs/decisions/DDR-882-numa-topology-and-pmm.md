= DDR-882 — NUMA topology + node-aware PMM (Group 3 item 17)

**Status:** Accepted — design; implementation lands in two gated slices
**Date:** 2026-08-09
**Scope:** `kernel/boot_info.h`, `kernel/mm/numa.{c,h}`, `kernel/mm/pmm.c`,
`kernel/main.c`, `smoke-numa`, `smoke-numa-alloc`.
**Blocks:** item 37 (per-CPU runqueue NUMA affinity) — it consumes
`numa_node_of_cpu()` and the per-node free lists defined here.

## 1. The NUMA struct lives in `boot_info.h`, but the BOOTLOADER does not fill it

The item says "full NUMA struct in `boot_info.h`". The struct goes there. It is
**not** populated by the bootloader, and that is a deliberate deviation from the
literal reading, stated here rather than made silently.

`struct boot_info` is a **packed struct with a flexible array member**
(`struct e820_entry e820[]`). Two consequences:

- Nothing can be appended after `e820[]` — a trailing flexible array must be
  last. A NUMA block placed there would overlap the E820 entries.
- It is written by `boot/stage2/stage2.asm`, **1294 bytes of 16-bit assembly**.
  NUMA topology comes from ACPI SRAT, which requires RSDP discovery, table
  checksum validation and sub-table walking. That is not something stage2 can
  do, and growing it to try would put the boot protocol at risk for data the
  kernel can read itself.

So: `struct numa_topology` is **declared** in `boot_info.h` (it is machine
topology, which is what that header describes) and **populated by the kernel**
from SRAT in `numa_init()`. The boot protocol is unchanged.

## 2. The ordering problem, and why the PMM re-buckets instead of seeding by node

`pmm_init()` runs at `main.c:2195`. `acpi_init()` runs at `main.c:2343`. **The
PMM has already seeded its buddy free lists before SRAT is readable.**

Rejected: moving `acpi_init` before `pmm_init`. ACPI parsing needs mapped memory
and is not written to run before the allocator exists; reordering two subsystems
this early, in the middle of an unresolved lost-thread investigation, is the
change most likely to produce a red that cannot be attributed.

**Adopted: seed as today, then re-bucket.** `numa_init()` runs after
`acpi_init()`, parses SRAT, and then walks the existing free lists once,
re-filing every block onto its node's list.

A block may **straddle** a node boundary. Re-bucketing splits any straddling
block into its two buddies and re-processes each, recursively, down to order 0 —
at which point a 4 KiB frame lies in exactly one node (SRAT ranges are page
aligned; a range that is not is rejected, see §4). The work is bounded: at most
one straddling block per node boundary per order.

## 3. Coalescing across nodes is prevented by construction, not by a check

`pmm_free_pages()` coalesces: it computes `buddy = addr ^ block_size(order)` and
calls `list_remove(order, buddy)`.

With per-node lists, both the free and the remove operate on
`numa_node_of(addr)`'s list. A buddy in a **different** node is simply not on
that list, `list_remove` fails, and coalescing stops — which is exactly the
required behaviour.

This is worth stating because it is the opposite of the usual mistake: no
explicit "same node?" test is added, because an explicit test is a thing that can
be wrong. The invariant falls out of which list is searched. The mutation matrix
in §5 checks that this reasoning is real and not merely plausible.

## 4. What is rejected rather than absorbed

The recurring defect in this codebase is a check that absorbs bad input instead
of refusing it. The specific cases here:

- **SRAT absent** → `numa.valid = 0`, one node covering all RAM. A
  single-node machine is not an error, and pretending to have topology derived
  from nothing would be worse than reporting none.
- **More than `NUMA_MAX_NODES` proximity domains** → refuse the extra domains
  loudly. Silently folding node 9 into node 0 would make allocations "succeed"
  on a node the caller never asked for.
- **A memory range that is not page aligned, or has zero length** → rejected and
  reported. An unaligned range breaks the order-0-lies-in-one-node property that
  re-bucketing depends on, so accepting it would corrupt the allocator's
  partitioning rather than merely mis-report topology.
- **A range whose node id exceeds the parsed node count** → rejected. It means
  the sub-table walk is out of sync, and continuing would file frames onto a
  list nobody allocates from — a silent memory leak of the entire range.

## 5. The gates, and what each is capable of failing

QEMU supplies topology with `-numa node,nodeid=N,memdev=...` plus
`-numa cpu,node-id=...`, so both gates run against a machine that genuinely has
two nodes rather than a parser fed a synthetic table.

**`smoke-numa` (slice 17a)** — topology. Asserts the parsed node count, the
per-node memory totals, and a CPU→node mapping. The memory totals are the
assertion with teeth: a parser that found SRAT but mis-walked the sub-tables
still reports a node count, and only the ranges show it read the right fields.

**`smoke-numa-alloc` (slice 17b)** — allocation. Asserts that
`pmm_alloc_pages_node(1, 0)` returns an address **inside node 1's range**. This
is the claim that matters: parsing SRAT proves nothing about allocation, and a
node-aware allocator that ignores its node argument passes every topology gate.

Planned mutation matrix (each must fail a gate, or the gate is not doing work):

| Mutation | Must be killed by |
|---|---|
| `pmm_alloc_pages_node` ignores `node` | `smoke-numa-alloc` |
| re-bucket files everything onto node 0 | `smoke-numa-alloc` |
| re-bucket does not split straddling blocks | `smoke-numa-alloc` |
| SRAT memory-affinity length field read at the wrong offset | `smoke-numa` |
| cross-node coalescing permitted | free-list integrity gate |

Per the standing rule, the harness is tested against a case it must **fail**,
not only cases it must pass.

## 6. Slices

- **17a — topology.** `boot_info.h` struct, `numa.{c,h}`, SRAT parse,
  `numa_node_of()`, `numa_node_of_cpu()`, `smoke-numa`.
- **17b — allocation.** Per-node free lists, re-bucket after `numa_init()`,
  `pmm_alloc_pages_node()` with documented fallback, `smoke-numa-alloc`.

17b depends on 17a. Item 37 depends on both. Item 17 is **not** complete until
both ship with their gates green and their mutants killed — reporting 17a alone
as "item 17" would be exactly the scope absorption this queue asks me to name.
