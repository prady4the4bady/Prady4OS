# DDR-969 — `aether_sectest` puts 10,432 bytes on a 16,384-byte kernel stack; and the "uninit PID" item is a non-bug

Status: ACCEPTED. Written before the code it governs (R16 / §NON-NEGOTIABLES 5).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

This DDR closes one BACKLOG item by **refuting** it, and opens a different,
measured defect found in the same function.

## 1. The "Uninit PID fix" backlog item is a non-bug — closed, not fixed

`CLAUDE.md` GROUP A carries:

> | Uninit PID fix | `AGENT_OOM_KILLED` path has uninitialised PID field — fix and verify |

and `SESSION_HANDOFF.md` cites it as corroborating evidence for OPEN-11:

```
AGENT_OOM_KILLED PID=2742943744      <-- garbage PID (0xA37Fxxxx)
AETHER_SEC_OOM_OK
```
> That PID is uninitialised — the `tcb-fields-not-zeroed` class from memory.
> The OOM path is being taken for real, and it reports a nonsense PID.

**It is not uninitialised. It is a named constant.**

```c
kernel/aether/aether.c:14
#define AE_TEST_PID 0xA37E0000u   /* a pid no real process uses, for self-test slots */
```

`0xA37E0000 == 2742943744` exactly. `log_pid` renders it with `kputdec`, so the
decimal form is simply that sentinel printed in base 10. The producing code is
`aether_sectest` arm (c), which builds a **`memset`-zeroed** throwaway TCB and
assigns `fake.pid = AE_TEST_PID` two lines before the charge — and the very next
line of the cited log, `AETHER_SEC_OOM_OK`, is that arm's success sentinel.
`smoke-aether-sec` requires `AGENT_OOM_KILLED` as an EXTRA_SENTINEL, so this line
is *supposed* to appear on every boot.

Two consequences:
- **There is nothing to fix.** The GROUP A item is removed as a non-bug.
- **A piece of OPEN-11's evidence is withdrawn.** "The OOM path is being taken
  for real" did not follow from this line; the self-test takes it every boot.
  OPEN-11's actual conclusion is unaffected — the eager 8 MiB stack was
  established independently by `pmmfree` measurement (DDR-943/944) — but the
  corroboration was spurious and should not be cited again.

The lesson is §0.7's in another guise: a value that *looks* like garbage
(`0xA37E0000`, high bits set, low 16 bits zero) is structured, and structure is
the clue. Nobody grepped the constant.

## 2. The real defect in the same function

`aether_sectest` declares its throwaway TCB **on the kernel stack**:

```c
struct tcb fake;
memset(&fake, 0, sizeof fake);
```

Measured, not estimated:

| quantity | value | source |
|---|---|---|
| `sizeof(struct tcb)` | **10,304 B** | compiled probe, kernel flags |
| `aether_sectest` frame | **10,432 B** | `sub $0x28c0,%rsp` in `build/aether.o` |
| kernel stack | **16,384 B** | `STACK_SIZE`, `sched.c:15` |
| **share of one kernel stack** | **~64 %** | — |

`struct tcb` is that large mostly because `FPU_STATE_MAX` is 4,096 (DDR-872,
sized for the full AVX-512 component set). The frame is a **2.4× outlier**: an
audit of every `sub $imm,%rsp ≥ 1 KiB` across all kernel objects puts the next
largest kernel frame at 4,432 (`sfs_set_tag`), then 4,176 (`sys_dmesg`), 4,160
(`sfs_get_tag`), 3,024 (`fs_test_thread`). Nothing else is close.

### Why the remaining headroom is thinner than 5,952 bytes looks

`aether_sectest` is called unconditionally from `main.c:2908` — every boot, not
behind `probe_enabled()`. `kmain`'s own frame is 96 bytes, so the chain is
roughly 10,600 of 16,384 at the deepest point of the self-test. What shares the
rest:

- everything `aether_sectest` calls (`aether_submit`, `aether_audit`,
  `aether_mem_charge`, the console path);
- **and any interrupt taken while it is on the stack.** `idt.c:65` sets
  `idt[v].ist = 0` for *every* vector, so there is no IST: the LAPIC timer,
  which fires continuously, pushes its frame and its whole handler chain —
  `sched_tick` → `schedule()` included — onto this same stack.

There is no guard page below a kernel stack and the build uses
`-fno-stack-protector`, so an overflow does not trap. `kstack_base` comes from
`kmalloc(STACK_SIZE)` (`sched.c:848`), so overrunning it scribbles **the kernel
heap** — silent corruption presenting later and elsewhere. That is precisely the
failure class this project has repeatedly spent sessions chasing.

### On §NON-NEGOTIABLES 3 ("no fix without a named mechanism from a real failing artefact")

That rule guards against speculative fixes to *observed intermittent failures* —
guessing at a red. This is the opposite case: no failure is being explained. The
artefact is a static measurement with exact numbers (10,432 of 16,384,
unconditional, interrupts enabled, no IST, no canary), and the mechanism is
named. **No claim is made that this has ever actually overflowed.** It evidently
has not, or boots would be corrupting the heap. The fix is a bound restored, not
a red chased.

## 3. Decision — heap-allocate the throwaway TCB

`kmalloc(sizeof *fake)` / `kfree`, keeping the `memset`. This is boot-time code
running long after the heap is up, it is called exactly once, and it removes
10,304 bytes from the deepest kernel frame in the tree.

Rejected alternatives:
- **`static struct tcb fake;`** — cheaper, but it is a writable global in a file
  whose whole purpose is self-tests, and it would be shared if this ever ran on
  more than one CPU. Not worth the 10 KiB of `.bss` or the precedent.
- **Shrinking `struct tcb`** — `FPU_STATE_MAX` is load-bearing (DDR-872, asserted
  against `CPUID.0xD`). Out of scope and not the defect.
- **Raising `STACK_SIZE`** — hides a 64 %-of-stack frame instead of removing it,
  and costs every thread in the system.

On allocation failure the two arms that need the TCB are **skipped with an
explicit line**, not faked. `smoke-aether-sec` requires `AGENT_OOM_KILLED` and
`AGENT_RATE_LIMITED`, so skipping makes the gate fail loudly — which is the
correct outcome, and strictly better than printing a success sentinel for a test
that did not run.

## 4. Verification bar

`smoke-aether-sec` green (all six sentinels), plus `smoke-shell` 5/5 and the
§HYGIENE set. The structural claim is re-checked directly: `aether_sectest`'s
`sub $imm,%rsp` must drop from 10,432 to a small frame in `build/aether.o`.
That disassembly check is the real proof — a green gate alone would not
distinguish the fix from no change, since the gate passed before too.

## 5. What would refute this

- The frame not shrinking in `build/aether.o` → the compiler kept a copy on the
  stack anyway and the change is cosmetic.
- `AETHER_SEC_OOM_SKIP` appearing in any run → the heap cannot serve a 10 KiB
  allocation at that point in boot, and the static object is the better trade
  after all.
