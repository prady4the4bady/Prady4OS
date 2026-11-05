# DDR-1199: the two unexplained surface failures, in one place

Status: **record and proposal, no code change.**
Asked by: operator, #45 comment 6594717280, step 5: "record `SURFDESTROY FAIL: reuse slot not reused` and the earlier `surface lifecycle` failure in one place, say whether either repeated, and propose logging."
Platform: QEMU on GitHub-hosted runners. Nothing here is verified on real hardware.

## 1. How the search was done

- **Which runs:** every failed `pradyos-ci` run created on or after 2026-10-28. That is 83 runs, and the listing is complete: the third page of results was empty.
- **What was read:** the log of each failed `build-and-boot` job. Each log was searched for `SURFDESTROY FAIL` and `FAIL: surface lifecycle`.
- **Why passing runs need no search:** both lines fail their gate (`SURFDESTROY FAIL` is a forbidden pattern in every gate, and the `surface lifecycle` line is the gate's own FAIL), so a passing run cannot contain either one.

## 2. `SURFDESTROY FAIL: reuse slot not reused`: seen twice

| Run | Branch, commit | Job | Gate |
|---|---|---|---|
| [40856880839](https://github.com/prady4the4bady/Prady4OS/actions/runs/40856880839) | `mut/ddr1181-t9c-e016` `a126766`, 2026-10-29 | shard 7 | `smoke-numa-steal`, gate 2 of 19 |
| [41367275758](https://github.com/prady4the4bady/Prady4OS/actions/runs/41367275758) | `c14-p3a-y1` `3f0d98d`, 2026-11-02 | shard 7 | `smoke-numa-steal`, gate 2 of 19 |

**What each log shows.** Both lines are identical, and neither branch changes surfaces: one is a DDR-1181 mutant, the other a DDR-1193 Y1 mutant. In both boots the serial output read, in order:

```text
PRADYOS_SURFACE_CLIENT_OK a=1 b=0
PRADYOS_TITLE_OK
PRADYOS_SURFDESTROY_CHURN_OK
SURFDESTROY FAIL: reuse slot not reused
```

**Where it comes from.** `user/surfdestroytest.c` step 2 does this:
1. creates surface A;
2. closes A;
3. creates surface B;
4. fails unless `b == a`.

`sys_surface_create` hands out the **lowest free** slot (`for (i = 0; i < SURFACE_MAX; i++) if (!g_surf[i].used)`). The test's own header says it runs alongside the compositor and `surfacetest`. In the logs above, `surfacetest` (`a=1 b=0`) prints its result just before.

**Likely mechanism, not proven.** Another surface user changes the table between A's close and B's create:
- `surfacetest` creates and closes surfaces: `make_window` creates each window (`SYS_SURFACE_CREATE`), and `main` later closes window A and then window C (`SYS_SURFACE_CLOSE`). A slot below `a` freed in that window makes `b < a`.
- A slot taken in that window makes `b > a`.

Either way `b != a` with no kernel fault. Two facts support this:
- **Both failures were in `smoke-numa-steal`.** It is the gate that boots two CPUs, one per NUMA node (`QEMU_NUMA=1`, `boot_test.sh` around line 280). With two CPUs the three processes **can** run in parallel; no log shows that they overlapped in these runs. Most other gates boot one CPU.
- **Neither branch touches `sys_surface.c` or the clients.**

The ids `a` and `b` are not printed, so the logs cannot confirm the mechanism. **The failure is unexplained, with one candidate. It is not a known kernel defect, and it is not closed.**

## 3. `[iso-user] FAIL: surface lifecycle`: not seen again

- **The original:** `2a49a73`, run [40230621123](https://github.com/prady4the4bady/Prady4OS/actions/runs/40230621123), shard 0 (DDR-1192 §1). It is before this search window.
- **The one hit in the window:** run [41196107835](https://github.com/prady4the4bady/Prady4OS/actions/runs/41196107835) on `mut/ddr1192-l1`. That is DDR-1192's own L1 demonstration, a branch whose `surfacetest` prints `PRADYOS_SURFACE_CLIENT_0K` on purpose, so it does not count.
- **Result:** no real recurrence since 2026-10-28. DDR-1192's evidence capture has therefore not yet been exercised on a real failure. The cause is still **unexplained**.

## 4. Proposed logging (not done here; each item needs its own PR)

1. **`surfdestroytest` step 2 prints the ids on failure.** For example `SURFDESTROY FAIL: reuse slot not reused a=<a> b=<b>`, using a small hex writer on the stack (the probe has no writable globals). It prints only after `b != a`, so it shows which way the ids differ (`b < a` or `b > a`). On its own it cannot tell a race from a kernel allocation fault; item 2 is needed for that.
2. **The kernel says who held the slot.** Under a diagnostic flag, `sys_surface_create` and `sys_surface_close` print `[surf] create id=<n> pid=<p>` and `[surf] close id=<n> pid=<p>`, off by default. With those lines, the next occurrence would show which process used the table between A's close and B's create.
3. **If (1) and (2) confirm the race,** step 2's assertion is wrong under concurrency, not the kernel. The fix would make the test's check true by construction, for example by asserting the slot is reused when no other surface user exists, or by gating the probe to run alone. That is a gate change, so it needs its own DDR, vacuity check and mutant. It must not weaken the test.

## 5. Honesty labels

- Read from CI logs and the code at `dev/phase1` `9105001`. Nothing here was run for this record.
- Both failures stay **open and unexplained**. Section 2's mechanism is a candidate only.
- This record changes no gate and closes no open defect (OPEN-1/2/12/13, U1–U3).
