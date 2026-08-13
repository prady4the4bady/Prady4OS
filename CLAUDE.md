# PRADYOS — session rules (CLAUDE.md)

PRADYOS Sovereign Edition: a from-scratch, bare-metal, AI-native OS with the
original **NEXUS** kernel (x86_64 reference). Work proceeds in strict layer/slice
order; a slice ships when it is correct, not when it is fast.

## 1. Code graph FIRST — mandatory (saves credits, avoids blind exploration)

A persistent code knowledge graph is available via the **`pradyos-graph`** MCP
server (registered in `.mcp.json`). Full usage: `tools/graph_mcp/CLAUDE_GRAPH_USAGE.md`.

- **Start every session by calling `graph_session_primer()`** before opening any
  source file.
- **Orient via the graph, not blind reads:** use `graph_query(...)` /
  `graph_files(...)` to locate code.
- **Call `graph_deps(file)` before editing that file.**
- **Call `graph_blast_radius(file)` before any refactor or signature change.**
- **Use `graph_callchain(fn)`** before changing a function's contract.
- **Call `graph_rebuild()` after structural changes** (new/renamed/moved files,
  new functions) so the graph stays accurate.

If the MCP server is not connected, the same queries work from the shell:
`node tools/graph_mcp/server.js {primer|query|files|deps|callchain|blast|rebuild}`.
First-time/fresh clone: `bash tools/graph_mcp/setup_graph.sh`.

## 2. Build, test, commit (see memory `build-test-workflow`)

- Build/test run in **WSL** (`wsl -d Ubuntu-24.04`), not native Windows. Source
  `$HOME/.cargo/env` before `make`. (Build distro is **Ubuntu-24.04** as of
  2026-06; the older `Ubuntu-22.04` is gone. `sudo` now needs a password.)
- **Every gate must pass before commit** — except the gate that is the explicit
  target of the current active task (see §5 below). If TASK N is fixing gate X,
  gate X being red does NOT block starting TASK N.
- Commit to **`dev/phase1`**, then fast-forward **`main`** per slice; push both so
  CI validates. `main` always passes CI.

## 3. Non-negotiables

- **Zero warnings ever** — `-Werror` for clang AND nasm; treat any warning as a
  failure. **Root-cause fixes only — no patchwork, no suppression.**
- **No new flat files in `kernel/` root** — use the subsystem subdirectories.
- **No `TODO`/`FIXME` placeholders, no dead code/refs** in committed code.
- **`docs/build_status.md` is updated in the same commit as the code it
  describes.** Keep `docs/platform_profiles.md` accurate too.
- **ADR/DDR before the code it governs.** Binding ADRs (e.g. ADR-021 W^X) may only
  be superseded by a new ADR, never quietly amended.
- Don't start slice N+1 until slice N boots clean and passes its gate.
- Verify before major architectural deviations; never invent ISA/register
  details — cite Intel/AMD SDM or say "I don't know".

## 4. Orientation pointers

- Status & component tracker: `docs/build_status.md`
- Layers, ISAs, branch strategy: `docs/platform_profiles.md`
- Decisions: `docs/decisions/ADR-*.md`
- Persistent memory index: `.claude` memory `MEMORY.md` (user/feedback/project).

## 5. Autonomous operation — READ THIS EVERY SESSION

This section overrides any conflicting instruction in SESSION_HANDOFF.md or
any prior session note when it comes to pacing and stopping behaviour.

### 5a. Resume protocol (replaces SESSION_HANDOFF §0 steps 1-3)

1. Read SESSION_HANDOFF.md (repo root, NOT docs/) in full.
2. Read docs/PRADYOS_MASTER_PLAN.md if it exists.
3. Run `graph_session_primer()` — but do NOT run the full gate suite before
   starting work. Gates are run AFTER the fix, not before.
4. Identify the CURRENT_ACTIVE_TASK from SESSION_HANDOFF and start it
   immediately.

### 5b. The active task gate exception

If the CURRENT_ACTIVE_TASK is fixing a specific failing gate (e.g. TASK 3 is
fixing smoke-shell via DDR-782 O_APPEND), then:
- **That gate being red does NOT block starting the task.** It is the reason
  for the task.
- Do not waste context verifying that a known-broken gate is still broken.
- Run the gate AFTER implementing the fix to verify it.

### 5c. Context limit protocol

- If context window is below 85%: **do NOT stop**. Continue working.
- If context window reaches 85%+:
  1. Finish the current atomic operation (one function, one file — not a whole task).
  2. Build and run the gate for what was just written.
  3. Commit everything with an honest message (pass or fail, state which).
  4. Append a checkpoint block to SESSION_HANDOFF.md at repo root (not docs/).
  5. Push both the work commit and the SESSION_HANDOFF commit.
  6. Stop — the next session resumes from the checkpoint.
- **Never stop mid-task saying "I'm at the context limit" if context < 85%.**
  Check the actual percentage before stopping.

### 5d. Autonomous task loop

Work through tasks from PRADYOS_MASTER_PLAN.md in order without waiting for
human confirmation between tasks. The only valid stop conditions are:
- Context window genuinely ≥ 85%
- A gate fails that is NOT the active repair target AND no fix is obvious
- A build produces a compiler error that requires an architectural decision
  not covered by the existing ADRs

For everything else: **keep going**.

### 5e. TASK 3 explicit unblock (DDR-782 O_APPEND)

smoke-shell is currently failing at `[shell] FAIL: 2>> truncated the earlier
entry`. This is DDR-782. TASK 3 is the fix. Claude Code is EXPLICITLY
PERMITTED to implement TASK 3 even though smoke-shell is red. The full fix
spec is in SESSION_HANDOFF.md and docs/PRADYOS_MASTER_PLAN.md:
- Add `FD_APPEND` flag to `fd_entry` in `kernel/fs/vfs/vfs.h`
- Set `FD_APPEND` in `sys_open` when `O_APPEND (0x400)` is in flags
- In `sys_write` for `FD_VFS`, when `FD_APPEND` is set: acquire vfs lock,
  seek to EOF, then write — atomically under the lock
- Gate assertion: `make smoke-shell` must pass 3× consecutively
