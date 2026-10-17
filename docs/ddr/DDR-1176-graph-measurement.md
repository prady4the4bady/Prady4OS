# DDR-1176: does the code graph (pradyos-graph) pay for itself? One measured task

Status: measured once. This answers operator item 8(c) of #45 comment
6229808666: "keep the graph as mandatory only if it wins".

## 1. Method

The fixed task is to plan adding `uint32_t foo` to `struct tcb`. That covers
the struct definition, the explicit initialiser (kmalloc does not zero),
fork/clone copy, the `_Static_assert`s on tcb offsets, and assembly that
hard-codes tcb offsets. It is planning only, with no edits.

Two fresh subagents with identical prompts ran it on 2026-10-17 against tip
`71d482e`. The only difference was the tool rule:

- **A** had to call the graph CLI (`tools/graph_mcp/server.js`) before
  opening any file.
- **B** was forbidden the graph and used grep and file reads only.

Token counts are the harness's `subagent_tokens`, read from each
completion notification.

## 2. Result

| Arm | Tokens | Tool calls | Graph calls | Answer quality |
|---|---|---|---|---|
| A, with graph | 676,403 | 3 | 2 (`query tcb`, `query offsetof`; the second returned nothing relevant) | Complete: struct, `sched_create_state` initialiser, both offset pins (`signal.c:28-33`, `syscall_entry.asm:58-59`), AP idle memset. Did not open `sched_create_user_clone`. |
| B, without graph | 676,416 | 3 | 0 | Complete, with the same items, plus the fork-inherit line in `sched_create_user_clone`. |

**Difference: 13 tokens, which is about 0.002%. The two arms used the same
number of tool calls and gave equivalent answers.** On this task the graph
did not win.

## 3. Why, and why this measurement is weak

About 676k of each count is **inherited context**: the session prompt,
including the 1.3 MB CLAUDE.md, which both subagents loaded. The task itself
cost only a few thousand tokens in either arm, so the measurement cannot
resolve a difference smaller than that inherited block. One task is also one
sample, and a cross-cutting struct-field change is exactly the case grep
handles well: the offsets carry distinctive names.

## 4. Decision

- **The graph is NOT kept mandatory on this evidence.** It is
  **recommended, not required**: use `graph_session_primer` at session start
  (cheap orientation) and `graph_blast_radius` before signature changes. Do
  not force a graph call before every file read.
- CLAUDE.md wording should change to match. That edit belongs in the cleanup
  PR (#45 item 4), or in a follow-up if that PR has already merged.
- **Re-measure after the CLAUDE.md shrink lands.** The inherited block will
  then be small enough to show a per-task difference. Use one task that grep
  handles poorly: "every caller of `sched_block_timeout` and what lock each
  holds" is the proposed task.

## 5. Not claimed

- No claim that the graph is useless. It was not shown to win on one task
  dominated by inherited context.
- No second graph tool was added.
