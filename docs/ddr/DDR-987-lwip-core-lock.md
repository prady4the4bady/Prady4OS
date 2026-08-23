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

```
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
