# DDR-734 — CAP_NET per-host allowlist (deny-by-default agent egress)

**Status:** proposed (pre-code)
**Layer:** 6 (AETHER authority) — live-agent hardening campaign, slice 1.
**Extends:** DDR-731 (CAP_NET), DDR-732 (/AETHER.CFG).

## Problem

CAP_NET (DDR-731) is all-or-nothing: a granted agent may `SYS_SOCK_CONNECT` to
**any host and port**. The sanctioned live-agent flow needs exactly one
destination (the Ollama endpoint at the SLIRP gateway, 10.0.2.2:11434); an
exploited agent should not be able to exfiltrate to arbitrary hosts just
because it legitimately holds CAP_NET.

## Decision — deny by default, sovereign-installed rules from config

- **Kernel:** a bounded allowlist (8 entries of `{host_be, port}`, `port 0` =
  any port on that host) in `sys_socket.c`. `sys_sock_connect` for a CAP_NET
  (non-sovereign) caller must match an entry; no match → audited `-EPERM`
  (`AR_CAP_DENIED`). **Empty list = deny-all for agents.** The sovereign
  operator bypasses the list (it is the authority that installs it).
- **New NSI `SYS_NET_ALLOW` (65):** `(host_be, port)` — sovereign-only, append
  an entry; `-EPERM` otherwise (audited), `-ENOSPC` when full. Install-only by
  design: rules cannot be removed or mutated at runtime (no revocation surface
  to abuse; policy changes are a config edit + reboot).
- **Config:** the daemon parses `net=<a.b.c.d>:<port>` lines from
  `/AETHER.CFG` (DDR-732 parser extended; multiple lines allowed) and installs
  each via `SYS_NET_ALLOW` before spawning any agent, printing
  `PRADYOS_NET_ALLOW_OK n=<count>`. The shipped default config gains
  `net=10.0.2.2:11434`, so `smoke-agent-live` (developer-run Ollama) keeps
  working unchanged.
- 65 is the next free NSI after `SYS_AGENT_METRICS` (64); `MAX_SYSCALLS` is
  already 80, no table change needed.

## Gate — `smoke-netallow` (79 gates)

Two witnesses in one boot, both deterministic:
1. **Kernel self-test** (kmain, alongside the other proofs): with a test entry
   installed, `netallow_check(allowed)` passes and `netallow_check(other)`
   fails, printing `[net] allowlist OK` (FORBIDDEN: `allowlist FAIL`). Covers
   match/deny logic without needing a live connection.
2. **Config-to-kernel path:** the daemon's `PRADYOS_NET_ALLOW_OK n=1` proves
   the `net=` line was parsed and installed via the sovereign-only NSI.

Regression: `smoke-capnet` (the CAP-less probe still gets `-EPERM` — now for
lack of CAP_NET, before the list is even consulted), `smoke-aethercfg`,
`smoke-aether*`, `smoke-net*`, then the full suite.

## Non-goals

- No CIDR/wildcard hosts, no DNS names — exact `host_be` match only (the guest
  has no resolver in the kernel path).
- No per-agent lists — one system list; per-agent scoping is a later slice if
  multiple live agents with different needs ever exist.
- No revocation NSI (see above).
