# DDR-981 — B#3 / OPEN-2 fixed: **`yield()` spun with interrupts masked**

Status: ACCEPTED — root cause named, fix shipped, gated.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

**Answers:** DDR-977 §5 ("what is still unknown — *why* an AP stops taking
interrupts"), which named four candidates and shipped no fix.
**Corrects:** the subsystem framing carried from B#3 through DDR-974/976/977.
It is not virtio-blk (DDR-974 got that right), and it is not the LAPIC either.

---

## 1. The answer in one paragraph

`SYSCALL` entry clears `RFLAGS.IF` (`syscall.c:229`, `wrmsr(MSR_SFMASK, 0x200)`)
and the entry path deliberately never re-enables it — `syscall_entry.asm:46`
says so in as many words: *"No nesting: SFMASK clears IF, so a syscall is never
interrupted."* Every yield-spin loop reachable from ring 3 therefore spun with
interrupts **masked**. `context_switch` preserves each thread's RFLAGS, so the
mask is carried *across* the context switch: two such threads on one CPU hand
off to each other through `schedule()` forever and that CPU never reaches the
idle loop's `sti; hlt` that would have cleared it. The CPU is not halted, not
starved, and not broken — it is **running normally with `IF` clear, permanently**.
Its LAPIC timer tick never arrives, `pc->ticks` freezes, and any virtio-blk
completion MSI-X routed to it is never serviced. That is B#3.

## 2. The measurement that named it

DDR-977 §5 asked for an instrument that says *where* a frozen AP is. This one is
an NMI: the BSP notices an AP whose `pc->ticks` did not advance across a whole
500-tick heartbeat and fires an NMI at it. NMI is the one interrupt that still
reaches a CPU with `IF` clear — which is exactly the state under test.

The AP's NMI handler does **not** print. It stashes into its own `struct percpu`
and the BSP relays it from the heartbeat, because a wedged AP may be holding
`g_line_lock` and printing from that context would deadlock the only CPU still
able to report.

First capture, `-smp 4`:

```text
[apfreeze] cpu=3 ticks=322 rip=0xFFFFFFFF800119FF cs=0x8 rflags=0x2 if=0
           lvt=0x20030 masked=0 svr=0x1FF swen=1 tpr=0x0 isr48=0 irr48=1 pid=89
```

Read field by field, this **refutes three of DDR-977 §5's four candidates and
confirms the fourth**, in a single line:

| field | value | what it rules out |
|---|---|---|
| `masked=0` (LVT bit 16) | clear | the timer LVT is **not** masked |
| `swen=1` (SVR bit 8) | set | the LAPIC is **not** software-disabled |
| `isr48=0` | clear | no un-EOI'd in-service timer interrupt; not a stuck ISR |
| `irr48=1` | **set** | a timer interrupt **is pending and undelivered** |
| `tpr=0` | zero | priority is not blocking it |
| `if=0` | **clear** | …and `IF` is the only thing left that can block it |

A pending IRR bit with a clear ISR bit, an unmasked LVT, an enabled LAPIC and a
zero TPR leaves exactly one possible blocker, and the dump reads it directly:
`RFLAGS.IF`. Candidate 1 — *"spinning inside a `cli` region that never
re-enables"* — is the survivor.

### 2.1 …then a second instrument, because one shot could not answer "running or spinning?"

A single sample cannot distinguish a CPU **spinning** at one address from a CPU
**running** and merely masked. So the probe samples the same CPU four times, one
per heartbeat, and walks the frame pointer (the kernel is built
`-fno-omit-frame-pointer`, and every link is bounded to the 16 KiB stack so a
garbage `rbp` cannot fault us inside NMI context):

```text
shot=1 rip=…11FBF pid=57 rsp=0x0110BCB0 bt=schedule,yield,mnt_lock,mnt_lock_live
shot=2 rip=…40FFE pid=57 rsp=0x07D87C98 bt=schedule_locked,schedule,yield,mnt_lock
shot=3 rip=…40FFE pid=89 rsp=0x0110BC98 bt=schedule_locked,schedule,yield,mnt_lock
shot=4 rip=…40FFE pid=57 rsp=0x07D87C98 bt=schedule_locked,schedule,yield,mnt_lock
```

The RIP walks, **the pid alternates between 57 and 89, and the RSP alternates
between two different stacks.** The CPU is *context-switching between two
threads* and making progress. It is not spinning on a lock and it is not wedged.
It is simply never interrupted. The full chain resolves to:

```
vfs_read → mnt_lock_live → mnt_lock → yield() → schedule() → context_switch
```

`mnt_lock` (`vfs.c:25`) is a yield-spin sleep-mutex. Two threads spin in it,
handing the CPU to each other, forever.

### 2.2 …then a third, to name the origin rather than the aftermath

`mnt_lock` is the victim, not the cause: a thread arrives there with `IF`
already clear. So: latch the **first** `yield()` each CPU ever executes with
`IF` clear, and record its return address. Two independent freezing boots:

```text
yrip=0xFFFFFFFF800155E5  ypid=25  yticks=181
yrip=0xFFFFFFFF800155E5  ypid=25  yticks=181
```

Identical address, identical pid, identical tick. `0x…155E5` disassembles to the
instruction after `callq <yield>` inside **`sys_yield`** — the ring-3
`SYS_YIELD` syscall. From there, `syscall.c:229` and `syscall_entry.asm:46` name
the mechanism outright.

## 3. Every affected call site

`yield()`'s callers split cleanly in two. `kernel/main.c`'s are kernel threads,
which already run with `IF` set — for them the fix is a no-op. Every **other**
caller in the tree is a syscall-context spin loop, and every one of them was
spinning masked:

| site | loop |
|---|---|
| `vfs.c:27` | `mnt_lock` — spin until another thread releases the mount mutex |
| `sys_io.c:57` | pipe write — spin until a reader drains the ring |
| `sys_io.c:268` | pipe read — spin until a writer fills it |
| `sys_io.c:293` | blocking console read — spin until COM1 RX has a byte |
| `syscall.c:155` | `sys_yield` |

Note the third and fourth rows: **PRISM's shell read loop is on this list.** The
condition each of these waits on is set by another thread or by an interrupt
handler, so a masked spin can never observe it. Every one of them was a livelock
by construction — the AP freeze is just the case where it became visible,
because on an AP the two spinners are the only runnable threads and idle's
`sti; hlt` is never reached.

## 4. The fix

In `yield()`, and only there — it is the single choke point every yield-spin
passes through. If `IF` is already set (kernel threads) nothing changes. If it is
clear, open a real interrupt window across the reschedule and close it again, so
the caller's SFMASK contract is restored byte-for-byte:

```c
uint64_t yf;
__asm__ volatile("pushfq; pop %0" : "=r"(yf) :: "memory");
if (yf & 0x200u) { schedule(); return; }     /* already interruptible */
__atomic_add_fetch(&g_yield_masked, 1, __ATOMIC_RELAXED);
__asm__ volatile("sti" ::: "memory");
schedule();
__asm__ volatile("cli" ::: "memory");
```

**Why this is the right place, and not `sys_yield`.** Fixing `sys_yield` alone
would not have fixed the observed livelock at all: the threads in the capture
are in `mnt_lock` under `vfs_read`, not in `sys_yield`. `sys_yield` was merely
the *first* masked yield, not the one that hung. Five call sites share the
defect and `yield()` is where they meet.

**Why not `sti` at SYSCALL entry.** That is the textbook fix and it is the wrong
one to make three days before a hard deadline: the entire syscall layer is
written against "a syscall is never interrupted" (`sys_exec.c:10` depends on it
explicitly for its CR3 swap), and making syscalls re-entrant is a change with a
blast radius across 149 gates. Recorded as post-1.0 work, not done here.

**Why it is safe.** `yield()` is thread context, never an ISR — no interrupt
handler in this tree calls it (they reach the scheduler via `sched_tick` →
`schedule()`). No `yield()` caller holds a spinlock across the call, and that is
not a new requirement: `vfs.c:20-24` already records that holding a spinlock
across a block deadlocks spinners, which is why `mnt_lock` is a sleep-mutex.
`schedule()` re-masks immediately via `local_irq_save()`, so the enabled window
is a few instructions inbound plus the resumed thread running interruptible
until the `cli`. A tick landing there is ordinary preemption, which `schedule()`
is already non-reentrant against — its own header comment explicitly
contemplates *"a voluntary schedule() [that] ran with interrupts enabled (e.g.
the yield in a driver's busy-wait)"*.

**There is in-tree precedent for exactly this pattern.** `virtio_gpu.c:78-95`
saves RFLAGS, `sti`s around each `hlt` of its used-ring wait, and restores on
return, for the same stated reason — *"this runs both at boot (IF=1) and inside
SYS_FB_FLUSH (IF=0 via SFMASK)"*. This generalises that one-driver workaround to
the choke point.

## 5. Result — 20 boots at `-smp 4`, with a denominator

§NON-NEGOTIABLE 2's 20× rule; §NON-NEGOTIABLE 17's denominator rule.

| | boots | boots with a frozen AP | `compl wait timeout` lines |
|---|---|---|---|
| before (DDR-977 instrument only) | 14 | **6** | 5–11 per frozen boot, 0 otherwise |
| after (this fix) | 20 | **0** | **0** |

The correlation before the fix is exact in both directions — a frozen AP always
produced timeouts, and no boot without one ever did — which is what makes the
after-column meaningful rather than merely quiet.

**And the fixed path is exercised, hard.** `ymask` (`g_yield_masked`, on the
heartbeat) lands at **~6.1 million per boot**, dominated by PRISM's console-read
spin. A green run with `ymask=0` would prove nothing; at 6.1M it is not possible
for the boot to have avoided the path. That is the denominator §NON-NEGOTIABLE 17
requires, and it is the reason the counter ships rather than being deleted with
the rest of the scaffolding.

Kernel under test: `d4b39c96a98ba2fead60d3eb23f37b9f4b5b739500f94f9eb401702552b83b22`
(R1). Logs under `build/gatelogs/apfreeze-fixed-*/`.

## 6. What ships, and what the instrument becomes

**The NMI probe stays**, as the regression detector — not as scaffolding:

- It prints **nothing** on a healthy boot. The only steady-state cost is a
  16-entry integer compare once per 5 s, with no UART output at all.
- That is precisely the property DDR-980 removed the `cputicks[]` heartbeat for
  lacking: its cost was per-CPU work plus ~30 characters of slow UART on *every*
  heartbeat, which is the hazard DDR-947 measured moving a failure rate from
  2/12 to 9/14. This is failure-path-only, the same discipline as DDR-977 §6.
- It is bounded: at most 4 NMIs per boot, at one CPU.
- An unsolicited NMI is **not** consumed — the handler acts only on a CPU the BSP
  armed, so a real machine NMI still reaches the panic path.

`[apfreeze]` is therefore now available as a `FORBIDDEN_SENTINEL`, which turns
"an AP froze" from an invisible flake into a named gate failure. DDR-980 §"lost
the always-on view" is answered: the detector no longer needs a device routed at
the frozen CPU to notice it.

## 7. What this closes, and one thing it does not

- **B#3** — closed. Named mechanism, named cause, fix, 20× gate.
- **OPEN-2** — closed for its block-touching gates, on DDR-977 §8.2's already-
  measured chain (frozen AP → unserviced completion MSI-X → `-EIO` →
  `[blk] multi-inflight FAIL` → gate red).
- **Not claimed:** that this is *every* OPEN-2 failure. DDR-977 §8.2 was careful
  to make the same reservation and it still stands — `smoke-crosswake` and
  `smoke-msixap` do no block I/O and could fail for their own reasons. With
  `[apfreeze]` now a gating sentinel, a recurrence names itself instead of
  hiding. Reopen on the first `[apfreeze]` line in CI.
- **Not claimed:** that DDR-974/976/977 were wasted. Each removed a wrong
  subsystem — virtio-blk, then MSI-X delivery, then the LAPIC — and DDR-977 §7.1
  is what forced the NMI rather than another block-path inference.

## 8. A blind spot this walked into, recorded

DDR-977's title says "CPU 3"; its §8 corrected that to "any AP". The same shape
recurred here: my first probe fired **one** NMI, which cannot tell spinning from
running-and-masked, and I only avoided concluding "it is spinning in `mnt_lock`"
because §2.1's repeat sampling showed the pid alternating. One sample of a
running system is a sample, not a state. The multi-shot arm ships for that
reason.
