# DDR-751 — PRISM system builtins: `uname` / `date` / `uptime` / `dmesg`

**Status:** proposed (pre-code)
**Layer:** user (PRISM) only. Consumes DDR-748/749/750; no kernel change.

## Problem

DDR-748/749/750 added `SYS_SYSINFO`, `SYS_TIME`, and `SYS_DMESG`, but their only
consumers are freestanding test probes. The interactive shell can't show any of
it — an operator at the `prism>` prompt cannot ask what machine this is, the
time, the uptime, or read the kernel log. Wiring these into PRISM realizes the
value of the last three slices.

## Decision

Four PRISM builtins, all over existing NSIs (no kernel change; all three
syscalls are uncapped, so they work from PRISM's ordinary ring-3 context):

- **`uname`** — `SYS_SYSINFO`; prints `uname: <vendor> "<brand>" cpus=<n>`.
- **`date`** — `SYS_TIME`; prints `date: YYYY-MM-DD HH:MM:SS` (musl `printf`
  handles the formatting — PRISM already links libc, unlike the freestanding
  probes).
- **`uptime`** — `SYS_SYSINFO`; prints `uptime: <secs>s` from
  `uptime_ticks / 100` (the tick is 100 Hz).
- **`dmesg`** — `SYS_DMESG` into a 4 KiB buffer; prints `dmesg: <n> bytes` then
  the captured log (`fwrite`). The byte-count header is the deterministic witness.

The `help` line and builtin dispatch gain the four commands. `poweroff`/`reboot`
are intentionally **not** added — those are CAP_SOVEREIGN (the compositor's `p`/`b`
keys); from unprivileged PRISM they would only ever print `-EPERM`, so a denied
stub would be noise, not a feature.

## Gate — extend `smoke-shell` (no new gate; stays 89)

Feed `uname`, `date`, `uptime`, `dmesg` before `exit`, and assert:
- `uname: ` line contains `cpus=` (proves `SYS_SYSINFO` round-tripped),
- `date: ` line matches `date: 20[0-9][0-9]-` (a plausible year — `SYS_TIME`),
- `uptime: ` line matches `uptime: [0-9]` (`SYS_SYSINFO` uptime),
- `dmesg: [1-9]` — a non-zero byte count (`SYS_DMESG` returned log data).

All four are deterministic (the values vary, but the shapes are fixed).

## Non-goals

- No flags (`uname -a`, `dmesg -w`, `date +FMT`); bare invocation only.
- No `poweroff`/`reboot` builtins (CAP_SOVEREIGN — compositor-only).
- No paging/scrolling of `dmesg` output; it dumps the recent-log window once.
