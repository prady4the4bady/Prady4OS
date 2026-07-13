# PRADYOS — session rules (AGENTS.md)

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
- **Every gate must pass before commit:** `make smoke smoke-fs smoke-fs-rw
  smoke-fs-sfs-rw smoke-fs-ext4 smoke-user` (and `toolchain-check`, `image`).
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
- Persistent memory index: `.Codex` memory `MEMORY.md` (user/feedback/project).
