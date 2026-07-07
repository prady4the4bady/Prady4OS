# DDR-726 — auto ambiance cadence + pre-transition pulse (with test knob)

> DDR before code (docs-first commit; implementation follows). The deferred
> "15-min-before pre-transition + 900 s auto cadence" item, designed so a gate
> can prove the FULL sequence in seconds.

## Decisions
- **D1 — time source:** the compositor reads elapsed time via the vDSO wall
  clock if user-mapped, else falls back to counting its own frames against the
  measured frame cadence (frames are timer-paced through SYS_YIELD). Verify
  which is available at implementation time; the DDR accepts either — the
  cadence only needs second-scale accuracy.
- **D2 — auto cadence:** every `C` seconds (default 900) the compositor
  advances the ambiance 0→1→2→3→0 through the EXISTING OKLab transition path
  (the same one the manual keys drive), announcing via the existing
  `PRADYOS_BACKDROP <name>` lines.
- **D3 — pre-transition pulse:** in the final 10% of each period (90 s at the
  default) the accent color pulses gently (the existing accent-flash helper at
  low amplitude), announced once per period as `PRADYOS_PRETRANSITION`.
- **D4 — test knob:** compositor hotkey `k` sets `C = 3 s` and prints
  `PRADYOS_CADENCE_TEST` — the gate's hook. After one full automatic cycle
  (4 advances) in either mode the compositor prints `PRADYOS_CADENCE_OK`.
- **D5 — gate `smoke-cadence`:** GPU boot + `input_inject` sends `k`; asserts
  `PRADYOS_CADENCE_TEST`, `PRADYOS_PRETRANSITION`, and `PRADYOS_CADENCE_OK`
  (a full auto cycle at the test cadence ≈ 12 s). 71 gates.

## Non-goals
Sun-position-driven schedules; per-ambiance dwell times; persisting the
cadence config (SFS `/etc/aether/config` stays deferred).
