# DDR-1003 — Compositor double-map `PTE_SW_SHARED` audit (Group E)

**Status:** AUDIT COMPLETE. Invariant HOLDS. One adjacent defect found, NOT fixed
here, with the reason and the artefact it is waiting on stated in §5.

---

## 1. The invariant under audit

`free_subtree` (`kernel/mm/vmm.c:266`) frees leaf pages at address-space teardown:

```c
else if (!(e & PTE_SW_SHARED))
    ptnode_free((void *)(uintptr_t)(e & PTE_ADDR));   /* user data page */
```

So the rule is exact, and it is a rule about *ownership*, not about the
compositor specifically:

> **Every leaf PTE installed into a user address space must either point at a
> frame that address space exclusively owns, or carry `PTE_SW_SHARED`.**

Violating it in the first direction double-frees a frame another address space
still maps. Violating it in the second leaks.

## 2. Every site that installs a user leaf — enumerated, not sampled

`vmm_map()` calls are excluded by construction: they map into the *kernel* master
tables (driver BARs, ECAM, LAPIC/IOAPIC, identity MMIO), and
`vmm_destroy_address_space` frees only PML4 slots that *differ* from the kernel
master, so kernel entries are never walked. That leaves `vmm_map_in`:

| site | frame | `PTE_SW_SHARED`? | verdict |
|---|---|---|---|
| `exec/elf.c:110` | segment page, `ptnode_alloc` | no | **correct** — private |
| `exec/elf.c:203` | user stack, `ptnode_alloc` | no | **correct** — private |
| `syscall/sys_surface.c:148,167` | compositor surface view | **yes** (`SURF_VIEW_FLAGS`) | **correct** |
| `syscall/sys_fb.c:47` | GPU scanout frames | **yes** | **correct** |
| `syscall/sys_io_uring.c:64` | ring page, `ptnode_alloc` | no | **correct** — per-thread, private |
| `syscall/sys_mmap.c:131` | anon page, `ptnode_alloc` | no | **correct** — private |
| `vdso/vdso_page.c:29` | the one vDSO frame | **yes** | **correct** |
| `aether/metric_page.c:41` | the one metric frame | **yes** | **correct** |
| `main.c:2790,2791,2954` | probe frames | no | **correct** — private to the probe AS |

The four genuinely double-mapped frames — surface views, framebuffer, vDSO,
metric page — all carry the bit. **No violation found.**

### 2.1 The allocator matches, which is a separate question and was checked

`free_subtree` frees with `ptnode_free`. Every privately-mapped frame above is
allocated with `ptnode_alloc`. A frame allocated by `kmalloc` and mapped without
the shared bit would be freed into the wrong allocator; there is no such site.

## 3. `fork` — the case that actually stresses the invariant

`vmm_cow_fork` (`mm/vmm_cow.c:76`) is where one frame becomes two mappings:

- `PTE_SW_SHARED` → *"share verbatim, no refcount, no COW."* Both address spaces
  carry the bit, so both skip it at teardown and nobody frees it. **Correct.**
- everything else → `pmm_incref(frame)`, and writable pages additionally become
  read-only + `PTE_SW_COW` in **both** parent and child.

And `pmm_free_pages` (`mm/pmm.c:188`) decrements at order 0 rather than freeing
outright, releasing only at zero. So two `ptnode_free` calls on a forked pair
release the frame exactly once.

**There is no double-free.** The audit's question is answered: yes, the
invariant holds, including across fork.

## 4. What the audit DID find: `ptnode_in_use` underflows on fork

`kheap.c` keeps an accounting counter:

- `ptnode_alloc` → `ptnode_in_use++` — once per **frame allocated**
- `ptnode_free`  → `ptnode_in_use--` — once per **mapping torn down**

Those are different quantities the moment a frame has more than one mapping, and
the COW fault does not reconcile them: `vmm_cow_fault` allocates the copy with
`ptnode_alloc` (`in_use++`) but drops the old reference with `pmm_free_page`
directly (**no** `in_use--`).

Trace one writable page that IS written by the child — this case is balanced,
which is why the bug is not obvious:

| step | `in_use` | refcount |
|---|---|---|
| parent `ptnode_alloc` | 1 | 1 |
| `fork` → `pmm_incref` | 1 | 2 |
| child writes → COW: `ptnode_alloc` copy, `pmm_free_page(frame)` | 2 | frame 1, copy 1 |
| child exits → `ptnode_free(copy)` | 1 | copy 0, freed |
| parent exits → `ptnode_free(frame)` | 0 | frame 0, freed |

Now the same page when the child never writes it:

| step | `in_use` | refcount |
|---|---|---|
| parent `ptnode_alloc` | 1 | 1 |
| `fork` → `pmm_incref` | 1 | 2 |
| child exits → `ptnode_free` | **0** | 1 — correctly not freed |
| parent exits → `ptnode_free` | **underflow** | 0, freed |

`ptnode_in_use` is `uint64_t` (`kheap.c:65`), so it wraps to ~2^64.

**This is not a corner case.** Read-only text pages are shared at fork with no
COW at all (`vmm_cow.c:93`, the `else` arm) and can never be written, so *every*
forked process underflows the counter by its text page count. `fork`+`exec` — the
common path — does this to nearly every shared page.

The frames themselves are freed correctly throughout. The defect is confined to
the counter.

## 5. Why this is NOT fixed in this commit

`kheap_outstanding()` is read by exactly two probes (`main.c:2834/2854` and
`main.c:2864/2883`). Both take a **difference** across a window in which nothing
forks, so a constant offset — even a wrapped one — cancels, and both still report
correctly. **No gate observes this today, so there is no failing artefact**, and
§NON-NEGOTIABLE 3 forbids the fix without one. Changing a memory-accounting path
days before a release, to correct a counter nothing currently reads, is also the
wrong trade on its own merits.

It is recorded rather than fixed because it is a loaded gun: the next person to
add a leak gate spanning a `fork` will get a wrapped counter and a mystery.

### 5.1 What the gate would have to do

`smoke-sharedpte` (the Group E gate, unbuilt) needs an arm that reads
`kheap_outstanding()`, forks a child that exits **without writing** a shared page,
reaps it, and reads the counter again. Under the current code the second read is
lower than the first — or wrapped — which is the artefact. Note the ordinary leak
shape (fork, child writes, both exit) is balanced per §4 and would **pass**: a
gate built the obvious way tests nothing here.

### 5.2 The fix, when there is an artefact

Make the two counters count the same thing. The narrow version: have
`ptnode_free` decrement `ptnode_in_use` only when `pmm_free_pages` actually
released the frame — which requires `pmm_free_pages` to report that, since today
it returns `void`. Do not "fix" it by having `vmm_cow_fault` call `ptnode_free`
instead of `pmm_free_page`; that would decrement on a path where no frame was
released and merely move the imbalance.
