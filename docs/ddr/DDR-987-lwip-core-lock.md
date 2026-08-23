# DDR-987 — the lwIP core has no cross-CPU lock, and that is OPEN-1 / OPEN-12

**Status:** ROOT CAUSE ESTABLISHED from an artefact. Fix designed here, implemented next.
**Date:** 2026-08-23
**Explains:** OPEN-1 (`smoke-surfdestroy` intermittent), OPEN-12 (intermittent
ring-0 panic), and — as a *candidate only* — OPEN-13.
**Supersedes:** DDR-985 §5's "cause unknown". **Relates to:** DDR-775 (the family),
DDR-979 (the CI capture), §INV.23.

## 1. The artefact

`build/gatelogs/panic/serial34.log`, run 34 of 40, kernel `d31b4023b0f74d06`
(hash verified against `build/kernel.elf` **before** resolving any address —
§NON-NEGOTIABLE 18).

```text
exception #GP   vector 0x0D   error 0x0   CS=0x08 (ring 0)   RFLAGS=0x82 (IF=0)
RIP  0xFFFFFFFF80047509   tcp_output
RAX  0xDDDDDDDDDDDDDDDD
backtrace: tcp_output <- tcp_connect <- psock_connect <- sys_sock_connect
           <- syscall_dispatch <- syscall_entry
```

Faulting pair:

```asm
mov 0x18(%r12),%rax     ; RAX = seg->tcphdr
mov 0x4(%rax),%edi      ; #GP -- RAX is non-canonical
```

`struct tcp_seg` (`tcp_priv.h:250`): `next`+0, `p`+8, `len`+16,
`oversize_left`+18, `flags`, **`tcphdr`+0x18**. The neighbouring
`movzwl 0x10(%r12)` (`len`) and `movl …,0x12(%r12)` (`oversize_left`) confirm
`R12` is a `struct tcp_seg *`.

`0xDD` is `POISON_FREE` (`kheap.c:22`), written by `cache_free`'s
`memset(ptr, POISON_FREE, c->obj_size)`. `lwipopts.h` sets `MEM_LIBC_MALLOC 1`
and `MEMP_MEM_MALLOC 1`, so **every** lwIP allocation — `tcp_seg` and `pbuf`
included — comes from `kmalloc`/`kfree`. The poison therefore proves the
memory was freed and is still being walked as live. **Use-after-free.**

## 2. The mechanism

`lwipopts.h:14-15`:

```c
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0   /* single core; lwIP calls are serialized */
```

lwIP has **zero** internal locking, on the stated assumption of a single core.
That assumption is stale: the kernel has been SMP since ADR-029/030/031 and the
gate runs `-smp 4`.

The port is not naive about re-entrancy — it guards five regions with a local
`cli` (`lwip_port.c:368,398,413,429,452`, in `psock_connect`, `psock_read`,
`psock_write`, `psock_close`, `net_init`):

```c
__asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
...lwIP core calls...
__asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
```

**`cli` masks interrupts on the current CPU only.** It is exactly the right
guard for one core and does nothing for the other three.

The other entry point has no guard at all — and it is the one that frees:

```c
void net_poll_tick(void) {        /* called from the TIMER ISR, idt.c:267 */
    sys_check_timeouts();         /* -> tcp_tmr: frees segs and pcbs */
    netif_poll_all();             /* -> RX -> tcp_input: frees segs on ACK */
}
```

So: **CPU A** in a socket syscall walks `pcb->unsent` inside `tcp_output`, while
**CPU B** takes its timer interrupt and runs `tcp_tmr`, or another CPU calls
`psock_close` → `tcp_abort` (which frees the pcb *and* its segment queues).
The segment is freed, poisoned, and CPU A reads `seg->tcphdr` out of it.

**`RFLAGS=0x82` (IF=0) in the dump is that very `cli`.** The fault happened
*inside* the protected region — direct evidence the guard was active and that
the interference came from another CPU, not from a local interrupt.

## 3. Why this explains everything DDR-775 could not

| observation (DDR-775 / DDR-985) | explained |
|---|---|
| `-smp 4` only, never `-smp 1` | `cli` is sufficient on one core |
| intermittent, ~2.4% (2 in 82 runs) | needs a narrow cross-CPU window |
| a **different** sentinel missing each time | the fault lands wherever the racing pair happens to be |
| both `#GP` and `#PF` | see below |
| the DDR-776 watchdog printed nothing | it is timer-driven; unrelated to this path |

**One bug, two exception types.** `0xDDDDDDDDDDDDDDDD` is **non-canonical**, and
x86-64 raises `#GP` for a non-canonical address but `#PF` for a canonical
unmapped one:

- the **seg** is freed → `seg->tcphdr` *reads poison* → non-canonical → **`#GP`**
  (this capture, and DDR-979's CI capture)
- the **pbuf** is freed but the seg is live → `tcphdr` is still a canonical
  pointer into freed memory → **`#PF`** (DDR-985 run 16)

## 4. The fix

One global lwIP core lock, held across every entry into lwIP core.

- `static spinlock_t g_net_lock = SPINLOCK_INIT;` in `lwip_port.c`.
- Replace all five `cli`/`popfq` pairs with
  `fl = spin_lock_irqsave(&g_net_lock)` / `spin_unlock_irqrestore(&g_net_lock, fl)`.
  These already save and restore RFLAGS, so the shape is unchanged and the
  existing same-CPU protection is *preserved* — `spin_lock_irqsave` disables
  interrupts locally as well as excluding other CPUs.
- Wrap `net_poll_tick`'s body in the same lock. This is the entry point that
  currently has none.

### Why a spinlock and not a mutex

One entry point is an **ISR**. A sleeping lock (the `mnt_lock` idiom,
`kernel/fs/vfs/vfs.c:27`) is illegal there. Verified: none of the five regions
yields or sleeps — `psock_read`'s `while` is a bounded ring-buffer copy, and the
longest hold, `net_init` → `net_loopback_tcp_test`, is a bounded 200-iteration
loop that polls the stack itself rather than waiting on the timer. Every one of
them already runs with interrupts off today, so this adds no hold time that was
not already there.

### Why `SYS_LIGHTWEIGHT_PROT 1` is NOT the fix

It guards only small critical sections (pbuf refcounts, `memp`), not whole core
operations. `tcp_output` walking a queue across many statements is exactly what
it does not cover. It would look like a fix and leave the race open.

### Lock ordering

`net -> heap`: lwIP allocates through `kmalloc`, which takes `g_heap_lock`.
`kheap` never calls into the network stack, so there is no cycle. Same shape as
the documented `mount -> blk` order.

## 5. Gate

`smoke-surfdestroy` is a poor gate for this — 2.4% means ~29 runs for even odds.
The honest position: this fix is justified by a **named mechanism from an
artefact** (§NON-NEGOTIABLE 3), not by a pass-rate delta a campaign of feasible
length can resolve.

- **Necessary:** the full `-smp 4` suite stays green, and a 40-run
  `smoke-surfdestroy` campaign produces **zero** `KERNEL PANIC` captures.
  20/20 and 39/40 were both observed *with the bug present*, so a green campaign
  alone proves nothing — it is a non-refutation, and must be reported as one (R17).
- **Mutation check:** revert the `net_poll_tick` lock only, and the race window
  widens; a targeted probe that hammers `connect`/`close` from two CPUs while
  the timer runs should fault far faster than 2.4%. That probe is the real gate
  and is the follow-up work.

## 6. What this does NOT claim

- **Not claimed: OPEN-13 is this bug.** A double free is what an unlocked
  free path produces, but `objsize=0x80` (128 B) does not obviously match
  `tcp_seg` (~32 B). Candidate only, pending its own artefact.
- Not claimed: that this is the *only* lwIP race. One lock closes the
  syscall-vs-timer and syscall-vs-syscall windows; it does not audit lwIP's
  callbacks (`ps_recv`, `ps_err`, `ps_connected`) for re-entrancy, which run
  from inside the lock and must not call back into locked entry points.
- Not claimed: any performance figure. A global lock over the stack is a
  throughput ceiling; measuring it is post-1.0 work.

---

## 7. The first cut was incomplete — two defects found in review

Recorded here rather than silently amended, because the *kind* of miss matters:
this DDR's whole thesis is "an unguarded lwIP entry point", and the first cut
**left another unguarded entry point**.

### 7.1 P0 — the virtio RX ISR was still bypassing the lock

`kernel/drivers/net/virtio_net.c:183` registers `net_complete` on **MSI-X vector
54**; `net_complete` (`:60`) calls `g_rx_cb(...)` at `:76`, and that callback is
`pradyos_netif_rx`, which runs `pbuf_alloc` / `pbuf_take` / `g_netif.input()`
(→ `ethernet_input` → `ip_input` → `tcp_input`) — **lwIP core, from an
interrupt, unlocked**. `net_irq` (INTx, `:90`) reaches the same code.

§4 locked the *timer* ISR and missed the *network RX* ISR, which is the more
frequent of the two. An RX interrupt on cpu B could enter `tcp_input` while cpu
A held `g_net_lock` in `tcp_output` — exactly the race this DDR exists to close.

**The fix has a trap.** The lock CANNOT go inside `pradyos_netif_rx`:
`net_fuzz_test()` (`:298`, `:306`) calls it from **inside `net_init()`'s locked
region**, and `g_net_lock` is not recursive — wrapping the callee would
self-deadlock at boot. So the lock goes in an ISR-only wrapper,
`pradyos_netif_rx_isr`, which is what `virtio_net_set_rx()` now registers; the
raw function stays for callers that already hold the lock.

Checked for the reverse recursion and there is none: `netif_poll_all` is lwIP's
own loopback-queue drain (`netif.c:1322`), not a virtio path, so
`net_poll_tick` → `netif_poll_all` cannot re-enter the RX callback. And a cpu
holding the lock has interrupts disabled locally (`irqsave`), so it cannot take
its own RX interrupt.

### 7.2 P1 — the proxy-socket slots were not serialized

Three TOCTOU windows, all outside the lock:

| site | window |
|---|---|
| `psock_connect` | scanned and claimed `g_ps[slot]` **before** acquiring — two cpus could pick the same `!used` slot, both `tcp_new()`, both return the same slot number: one pcb leaked, two owners for one `proxy_sock` |
| `psock_read` / `psock_write` | validated `used` / `state` / `pcb` **before** acquiring — `psock_close` on another cpu could retire the slot in between, so the loop read a freed RX page or `tcp_sndbuf()` ran on a detached pcb |
| `psock_close` | freed `s->rx` **after** releasing, while `used` was still 1 — a concurrent `psock_read` could copy out of a page already returned to the PMM |

All four now claim, validate and retire **under** the lock; page frees moved
after release (never free while holding it). Lock order gains `net -> pmm`,
consistent with the existing `net -> heap` (kmalloc reaches the PMM anyway).

### 7.3 What review confirmed as already sound

- `psock_connect` has one acquire and one release on each of its two paths.
- `ps_recv` / `ps_err` / `ps_connected` do not re-enter a locked `psock_*` /
  `net_*` entry point; their lwIP calls run inside the caller's critical section.
- No `heap -> net` edge exists, so the `net -> heap` order has no cycle.
- No yield, scheduler wait, `copyin` or `copyout` inside any locked region.

### 7.4 The lesson worth keeping

Two independent misses in one change, both of the same shape: *an entry point
nobody enumerated*. Before declaring a lock complete, enumerate entry points
**from the callee outward** — every registration (`msix_register`,
`virtio_net_set_rx`, `irq` hooks), not just the call sites that are easy to grep
from the top. §INV.23's sibling lesson: a guard that is present is not the same
as a guard that is sufficient.

---

## 8. The lock itself caused a regression — a long hold stalls other CPUs

Caught by `smoke-surfdestroy` on the §7 build, before anything was pushed:

```
[vblk] compl wait timeout unit=0 dest_cpu=1 dest_dticks=0 dest_abs=312
       bsp_abs=312 dest_present=1 ticks[331,312,311,308] on_cpu=1 lba=3
[smp] blk integrity FAIL checksum-mismatch done=0x807 spawned=4/4 prog=64,64,64,54
```

`dest_dticks=0` — the destination cpu stopped advancing its tick counter during
the wait. That is the `compl wait timeout` signature DDR-981 was supposed to have
retired, reappearing on a build that carries DDR-981. It is not a DDR-981
recurrence: **this lock caused it.**

### The mechanism

`kernel/idt.c:259-267` increments `g_ticks` and then calls `net_poll_tick()`
**inside the timer ISR**. §4 made `net_poll_tick` take `g_net_lock`. A cpu that
blocks there spins with interrupts disabled, so it cannot take its *next* timer
interrupt and its per-cpu tick counter freezes. virtio-blk's completion deadline
is tick-bounded, so it expires and the read returns `-EIO` → checksum mismatch.

And §4 gave it something long to block on: `net_init()` held the lock across
`net_loopback_test()`, `net_loopback_tcp_test()` (a **200**-round pump) and
`net_fuzz_test()` (a **256**-round loop). Under TCG that is hundreds of
milliseconds with every other cpu's timer ISR spinning.

### The error in §4's reasoning, stated plainly

§4 considered exactly this and cleared it with:

> "every one of them already ran with interrupts off today, so this adds no hold
> time that was not already there."

That is true for the cpu **holding** the lock and false for every **other** cpu.
`cli` disables interrupts locally and never blocked another core; a global
spinlock does. The hold time was indeed unchanged — what changed is who waits on
it. A guard's cost is not only what it stops the holder doing.

### The fix: never hold across a poll loop

- `net_init()` releases the lock **before** the three self-tests.
- Each test takes it in short bursts: `net_pump_locked()` (one
  `netif_poll_all` + `sys_check_timeouts` per iteration), `net_timeouts_locked()`,
  and `pradyos_netif_rx_isr()` for the fuzz frames.
- Bounded setup/teardown inside the tests (`udp_new`/`udp_bind`/`tcp_new`/
  `tcp_connect`/`pbuf_free`/`udp_remove`) keeps its own short critical section,
  so no lwIP call is left unlocked.

Verified by grep that every remaining bare `sys_check_timeouts`,
`netif_poll_all` and `pradyos_netif_rx` sits inside a locking helper or
`net_poll_tick`.

### The rule this earns

**Never hold a lock that an ISR contends for across a bounded-but-long loop.**
"Bounded" is not "short". `net_loopback_tcp_test`'s 200 rounds are bounded and
still long enough to expire a 5-second block deadline on another cpu. When an
interrupt handler can wait on a lock, the hold time budget is set by the most
timing-sensitive thing any *other* cpu does in its ISR — here, advancing ticks.

### Measurement

`smoke-blk-integrity` x6 and `smoke-surfdestroy` x6 on the fixed build, counting
`compl wait timeout` occurrences per boot. Prior evidence for comparison: the
§7 build produced the timeout above on its first `smoke-surfdestroy`, and
DDR-981's own campaign measured **0** timeouts in 20 boots at `-smp 4` before
this lock existed.

---

## 9. Second review round — three more defects, one dispute

### 9.1 `psock_close` did not revalidate under the lock

§2 introduced the rule "revalidate inside the lock" and applied it to
`psock_read` and `psock_write` — and not to `psock_close`. The window it leaves:

1. cpu A passes the pre-lock `used` test.
2. cpu B closes and retires the slot.
3. cpu C connects and claims the **same** slot.
4. cpu A takes the lock and tears down **cpu C's** pcb and RX page.

Fixed. This is the third defect in this DDR of the same shape: a rule stated
correctly and then applied to some of its sites. §2 missed the RX ISR, §8 missed
the other CPUs, §9.1 missed the third `psock_*` entry point. **Enumerate the
sites, then apply; do not apply while enumerating.**

### 9.2 A failed `tcp_connect()` leaked the pcb, the slot and the RX page

lwIP's contract: only `ERR_OK` transfers pcb ownership to the stack; on any
error the attempt was never enqueued and the **caller still owns the pcb**. The
old path did:

```c
if (e != ERR_OK) { s->state = PS_ERR; return -1; }
```

— leaving the pcb allocated, `used` still 1, and the RX page unfreed, forever.
Pre-existing (the `cli` version did the same), not a regression from §4.

Fixed: `tcp_abort(s->pcb)` and full slot rollback under the lock, page freed
after release.

### 9.3 Disputed: "sync the current-state values to §INV.14 / §INV.18 / §INV.4"

The review is right that §CURRENT BUILD STATE contradicts those invariants, and
right that a wrong NSI record risks a duplicate allocation. It is wrong about
which side is stale. Measured:

| value | §INV said | build state said | **measured** | source |
|---|---|---|---|---|
| NSI max | 74 | 93 | **95** | `kernel/syscall/syscall.h:170`, shipped by `user/prism.c:30` |
| `kernel.bin` | 1,053,054 B | 1,065,350 B | **1,065,350 B** | `stat` on the tested build |
| DDR free | 936+ | 987+ | **988+** | `ls docs/ddr docs/decisions` |

Reverting NSI to 74 — the change the finding asked for — would have been the
worst of the three options and would have caused exactly the duplicate-NSI harm
it warns about. All three now carry measured values, and §INV.14/18/4 were
corrected rather than the fresh lines being overwritten.

**The general point.** An invariant block is only authoritative while someone
maintains it. §INV.14 had drifted 21 NSI numbers behind the header it describes.
When an invariant and a measurement disagree, measure again — do not assume the
document labelled "hard-won" is the current one.

### 9.4 Deferred, and NOT this PR's to fix: slot-reuse across the ownership check

`sock_denied()` (`kernel/syscall/sys_socket.c:64`) reads `g_sock_owner[slot]`
with no lock; the syscall then calls `psock_read/write/close` afterwards. Between
the two, another cpu can close and reuse the numeric slot, so the operation lands
on a **different connection** than the one whose ownership was checked. A slot
index alone cannot identify a connection across reuse.

This is real and it is a CAP_NET isolation defect (DDR-731). It is **not** a
regression from this DDR — the window predates it, and §2's move of the claim
inside the lock narrowed it rather than widening it.

The fix is a generation-bearing handle: a `gen` on each `proxy_sock`, bumped on
claim, carried in the handle the syscall layer holds, and validated together with
the owner under `g_net_lock` in all three operations. That changes the
slot-handle contract across the syscall/lwIP-port boundary, so it is recorded
here for a decision rather than taken unilaterally inside a PR whose remit is the
ring-0 panic.
