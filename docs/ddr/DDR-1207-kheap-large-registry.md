# DDR-1207: the kernel heap's large-allocation registry is too small for a thread storm (#135)

Written 2026-11-18, before the code, on `dev/phase1` `a50e509`. Operator #45 comment 6983608675 asked for #135 to be measured (step 3). This record fixes the mechanism that measurement named.

## 1. The failing artefact and the mechanism

- **The failure.** `smoke-thread` arm R fails with `PTHR FAIL: R create`. With #141's step print (DDR-1197 Amendment 2), every local failure reads `[pthr] R create rc=-12 step=6`: `SYS_THREAD_CREATE` returned `-ENOMEM`. That return means `sched_create_state` got 0 from `kmalloc`.
- **What fills.** A temporary print in `kmalloc`'s large path (not committed) showed, in all 8 instrumented failures, `large registry FULL in_use=256 order=2`. The page allocator never failed.
  - `kheap.c` keeps every allocation above 2 KiB in a fixed table, `large[MAX_LARGE]`, with `MAX_LARGE = 256`. `kfree` needs the table to learn the block's order.
  - Each thread holds two entries: its `tcb` (10 304 B, an order-2 block) and its kernel stack (`STACK_SIZE` 16 KiB, order 2).
  - Both are freed only after the thread's RCU grace period (DDR-1200 D5). Arm R creates and joins 200 threads in a row, and the frees fall behind.
- **How close a passing run comes.** Every passing local run peaked at 240 or 256 entries (`large hw`). `dev/phase1` runs within one or two threads of the cap.
- **Rates.** Arm R failed 2 of 15 times locally on `dev/phase1` and 9 of 15 times on #140's head (p = 0.021). In CI it failed 0 of 117 times on trees without #140's P4, and 2 of 6 with it (p = 0.002). Counts are in #135 (6987460912). Why P4 raises the rate is not shown; this record does not need it, because the cap fails on both trees.

## 2. Decision

- **D1. `MAX_LARGE` becomes 1024.**
  - The table grows by 768 entries of 16 B: 12 KiB of `.bss` and no `.text`. `.bss` is not in `kernel.bin`, so the stage-2 size gate does not move. DDR-1183's 2 MiB PT_HI runtime ceiling had 169 802 B to spare at 52 chunks, so 12 KiB is inside it.
  - With about 100 entries in steady use, 1024 leaves room for about 460 threads waiting for their grace period, four times what arm R's storm reaches.
- **D2. The cost.** `kmalloc` and `kfree` scan the table linearly under the heap lock with interrupts off. At 1024 entries a full scan reads 16 KiB, four times today's worst case. No index is added: measured peaks are near 256, and a scan stops at the first free slot or the match.
- **D3. An exhaustion names itself.** When the table is full, `kmalloc` counts it under the lock and, after releasing the lock, prints once: `[kheap] large registry full in_use=<n>`. It prints outside the lock, so the console is never taken under the heap lock. It is not a forbidden pattern: a full table is a refusal that callers already handle.
- **Not taken (stated):** moving `tcb`s and kernel stacks out of the table into explicit-order page allocations. That would remove the thread limit from the table entirely, but it changes the thread create and free paths, where OPEN-2 lives, for a capacity problem a constant solves. It remains the better long-term design if the table is ever the limit again.

## 3. Gate: arm KL in `smoke-thread`

- **Arm KL.** When `QEMU_PROBES` includes `thread`, the boot path calls `kheap_large_selftest(300)` before it launches the thread probe. The test `kmalloc`s 300 blocks of 16 KiB, counts the successes, frees them all, and prints `PRADYOS_KHEAP_LARGE want=300 got=<n> ok=<0|1>`.
  - The sentinel is `PRADYOS_KHEAP_LARGE want=300 got=300 ok=1`, appended to `smoke-thread`'s `EXTRA_SENTINEL`.
  - 300 alone exceeds the old cap, whatever else is in use. 300 + steady use (about 100 at that point) stays under 1024.
- **Vacuity, read from the code before writing:** on `dev/phase1` the 257th entry cannot be had, so `got` is at most 256 minus the entries already in use, and the arm is red.
- **Mutant K1:** `MAX_LARGE` back to 256. It must be red on KL in CI on the final head.
- Both lines are emitted as one `kline` each, so another CPU's output cannot split the sentinel the gate matches (CodeRabbit on #142). The gate's post-run `grep` also prints them.
- **Arm R** stays as it is. It is the real-world check, and #135 stays open until it is green over repeated CI runs.

## 4. Local results (QEMU TCG, one boot each; CI on the head is the check)

- **The fix:** `PRADYOS_KHEAP_LARGE want=300 got=300 ok=1`; arm R `rounds=200 leak_ok=1 conclusive=1`. `smoke-shell` passes 5/5 and `hygiene_check.sh` all eleven checks.
- **Mutant K1** (`MAX_LARGE` 256): red on KL (`got=193 ok=0`). D3's line printed `[kheap] large registry full in_use=256`. Arm R also failed (`rounds=0`), a second witness.
- **`kernel.bin`:** 1 683 850 B, unchanged. The change is all `.bss` and page-padded `.text`.

QEMU (TCG, 4 vCPU) only; not verified on real hardware. This record does not close OPEN-1/2/12/13 or U1–U3.
