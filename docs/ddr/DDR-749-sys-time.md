# DDR-749 — `SYS_TIME`: ring-3 wall-clock date/time

**Status:** proposed (pre-code)
**Layer:** syscall + user probe. Sibling of `SYS_CLOCK` (DDR-709) and `SYS_SYSINFO`.

## Problem

`SYS_CLOCK` (DDR-709) returns only *seconds since midnight* — enough for the
time-of-day ambiance, but a program cannot learn the date or the wall-clock time
in a usable broken-down form. The RTC driver already reads full date/time
(`rtc_now` → `struct rtc_time {year, month, day, hour, minute, second}`); nothing
exposes it to ring 3.

## Decision

**`SYS_TIME` (NSI 72)** — `(struct rtc_time *out) -> 0 | -EFAULT`. No capability
(the wall clock is non-sensitive, like `SYS_CLOCK`). The handler calls `rtc_now`
into a local `struct rtc_time` and `copyout`s it verbatim (POD: `u16 year` +
five `u8` fields). Ring 3 mirrors the struct.

## Gate — `smoke-time` (new; 87 → 88)

A freestanding probe `user/timetest.c` (musl-free, `user.ld`, default root) reads
`SYS_TIME`, formats `TIME YYYY-MM-DD HH:MM:SS` to the console for visibility, and
**range-validates** each field — `year ∈ [2020,2100)`, `month ∈ [1,12]`,
`day ∈ [1,31]`, `hour < 24`, `minute < 60`, `second < 60`. On all-pass it prints
`PRADYOS_TIME_OK`, else `TIME FAIL`. The gate asserts `PRADYOS_TIME_OK`
(EXTRA_SENTINEL) with `TIME FAIL` forbidden. This is **deterministic**: the exact
value is the (host-provided) RTC time and cannot be asserted, but the field
ranges always hold, so the gate never flakes on time drift.

## Non-goals

- No timezone / UTC-offset handling — the RTC value is reported as-is (QEMU's RTC
  is host-local by default), matching `SYS_CLOCK`.
- No settable clock (`SYS_SETTIME`), no monotonic-vs-wall distinction (the vDSO
  monotonic clock is separate), no sub-second precision.
- No PRISM `date` builtin yet (keeps this slice off the shell); a shell surface
  can follow.
