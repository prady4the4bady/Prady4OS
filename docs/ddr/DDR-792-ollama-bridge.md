# DDR-792 — `aether/ollama_bridge/` design

**Status:** design only. **No code is written by this DDR.** The directory stays
`.gitkeep`-only until this design is accepted.
**Date:** 2026-07-28
**Depends on:** D-03 ensemble router (shipped), B-02 audit log (shipped),
I-01/I-08 single-inference-path gate (shipped).

## Why this directory exists at all

I-01/I-08 makes the D-03 router the only inference path, and forbids any module
under `aether/agents/` from importing an HTTP client. That rule needs somewhere
for the HTTP to legitimately live. `ollama_bridge/` is that place and the only
one: it is the single module permitted to speak the wire protocol to a local
Ollama server.

Concentrating it is the point. Model traffic scattered across agents cannot be
rate-limited, cannot be audited uniformly, and cannot be blocked by privacy mode
— because each site would have to remember to do all three.

## Scope

**In:** HTTP transport to a local Ollama endpoint, retry, timeout, streaming
assembly, error translation, and the per-call audit record.

**Out:** model selection (D-03 owns it), prompt construction (callers own it),
cloud endpoints (`cloud_bridge/`, blocked — see DDR-793), and any retry policy
decision that belongs to the caller (see "what is not retried").

## Retry policy

The rule that matters: **retry transport, never semantics.**

| condition | retried | why |
|---|---|---|
| connection refused / reset | yes | the server is restarting; the request never ran |
| DNS / socket timeout before first byte | yes | as above |
| HTTP 5xx | yes | server-side, request may not have been processed |
| HTTP 429 | yes, honouring `Retry-After` | explicit backpressure |
| read timeout **after** first byte | **no** | the model is generating; retrying doubles the cost and may double a side effect |
| HTTP 4xx other than 429 | **no** | the request is wrong; retrying sends the same wrong request |
| model-not-found | **no** | a routing error — surface it to D-03 so the profile can be marked unavailable |

- **Attempts:** 3 total (1 initial + 2 retries).
- **Backoff:** exponential from 250 ms, factor 2, full jitter. Jitter is not
  cosmetic: without it, N agents that fail together retry together and
  synchronise into a thundering herd against a single local server.
- **Budget:** the retry sequence must fit inside the overall timeout below.
  A retry policy that can exceed its own deadline is how a "30 s timeout" becomes
  a 90 s stall.

## Timeout

**30 s total, per call, wall-clock, covering all attempts.**

It is a *deadline*, not a per-attempt timeout, for the reason above. Split as:

- connect: 5 s
- first byte: 15 s (a local model that has not started emitting by then is
  loading or wedged)
- total including retries and streaming: 30 s hard cap

On expiry the bridge raises a timeout error. It does **not** return a partial
completion as if it were whole — a truncated answer that reads as complete is
worse than a failure, because every downstream consumer (D-07 hypotheses, D-11
beliefs) would record it as a real result.

## Audit schema

One record per call, written through B-02, matching the shape D-03's
`route.call` already uses so the two can be joined:

```
ddr:          "DDR-792"
file:         "aether/ollama_bridge/<module>.py"
action:       "ollama.call" | "ollama.retry" | "ollama.failed"
gate_status:  "ok" | "retried" | "timeout" | "refused"
extra:
  model            str      the model actually asked for
  endpoint         str      host:port, never a full URL with query
  attempt          int      1-based
  latency_ms       float    this attempt
  total_latency_ms float    all attempts, on the terminal record
  token_count      int      prompt + completion, provider-reported when available
  prompt_sha256    str      hash, NOT the prompt
  truncated        bool     whether the deadline cut generation short
  error_kind       str      on failure: "connect" | "timeout" | "http_4xx" | ...
```

**Prompts are hashed, never logged.** The audit log is append-only and widely
read; putting prompt text in it turns every audit reader into a reader of
whatever a user typed. The hash is enough to prove two calls were identical,
which is the question the log actually needs to answer.

## Invariants this must satisfy

- **S2** — the shared rate limiter applies here; the bridge does not get its own
  quota. A bypass path with its own limit is not a limit.
- **S5** — every call appends; nothing is edited or removed.
- **S9** — retry and backoff are computed from injected clock and jitter
  sources, so the policy is testable without sleeping.
- **S14** — this module must not import from `aether/agents/`. The dependency
  runs one way: agents → D-03 → bridge.

## Discriminating gates the implementation must pass

1. A read timeout **after** first byte is not retried — a naive "retry on any
   timeout" passes every other test and fails this one.
2. The 30 s deadline covers retries: a call whose retries would exceed it fails
   at 30 s, not later.
3. A partial stream at deadline raises rather than returning the partial text.
4. Backoff jitter is present: two bridges failing simultaneously with the same
   seed-free config must not produce identical retry timestamps.
5. The audit record contains `prompt_sha256` and **no** prompt text — asserted
   by scanning the written record for a sentinel string in the prompt.
6. A 4xx is surfaced, not retried, and marks the model for D-03.

## What is deliberately not decided here

Connection pooling and concurrency limits. Both depend on measured behaviour of
the local server under the real agent fleet, and guessing them now would bake in
numbers nobody has evidence for. They get their own DDR once I-10's daemon is
running a real fleet.
