import io
p = "docs/AETHER_MASTER_FEATURES.md"
s = io.open(p, encoding="utf-8", newline="").read()
anchor = "### Boot / Kernel / Drivers / Filesystem\n"
assert s.count(anchor) == 1
add = (anchor +
"- **ADR-038 demand-paged user stack** — eager window `USER_STACK_EAGER_PAGES=8`,\n"
"  `#PF`-driven growth, guard page below `USER_STACK_BOT` (gate: `smoke-stack-demand`).\n"
"  SHIPPED 2026-08-20: three consecutive CI greens on one SHA `bb19449` —\n"
"  runs 32395650883, 32402456422, 32405858449. `main` fast-forwarded to `bb19449`.\n"
"- **DDR-955 `sched_block_timeout`** — bounded-wait primitive: TCB `block_deadline`/\n"
"  `wake_timed_out`, expiry scan in `sched_tick` over the `->next` ring under\n"
"  `irq_save()`, waking via `sched_unblock` outside the walk (gate:\n"
"  `smoke-blk-timeout`, 20/20, forbidden-sentinel arm on both timeout strings).\n"
"  **PARTIAL — 2 of 4 sites bounded.** virtio-blk slot-wait and compl-wait at 500\n"
"  ticks are shipped. `ipc_recv` and `bcast_wait` are deliberately NOT bounded:\n"
"  `bcast_wait` returns `void` and fills an out-parameter, so an early timeout\n"
"  hands the caller an unfilled buffer it cannot detect, and `ipc_recv`'s `-1`\n"
"  already means \"cap denied\". Both need a return-value contract first; measured\n"
"  at 19/20 with two lwIP `#GP` panics before revert.\n")
s = s.replace(anchor, add, 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("Section A updated")
