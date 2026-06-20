# PRADYOS Code Graph — mandatory usage (read this first)

A persistent, queryable knowledge graph of the PRADYOS codebase. It exists so a
session can **orient and navigate without blind file reads** — saving credits and
avoiding wrong assumptions. Treat the graph as the first lens on the code.

## What it is

- A Node.js **MCP server** (`tools/graph_mcp/server.js`) exposing `graph_*` tools.
- Backed by a local **SQLite** database (`sql.js`, file `.graph/graph.sqlite`,
  gitignored) built by parsing the repo with **Tree-sitter** (C, Rust) and a
  focused NASM symbol extractor.
- **Pure JS/WASM** stack (`sql.js` + `web-tree-sitter` + `tree-sitter-wasms`) — one
  `node_modules` works on the Windows host, WSL, and Linux CI; no native build,
  no `node-gyp`, no per-platform prebuilds.
- **Incremental**: only files whose content hash changed are re-parsed. Startup
  after the DB exists is ~1 s; a from-scratch index of the kernel is ~1–2 s.

## Mandatory session workflow

1. **Start every session with `graph_session_primer()`** — subsystems, key entry
   points, gates. ~2 KB, fits well under 3000 tokens. Do this before opening files.
2. **Before opening files to orient**, use `graph_query("…")` / `graph_files("…")`
   to locate the right symbols/files. Don't sweep directories blindly.
3. **Before editing a file**, call `graph_deps("path")` — see what it includes,
   who includes it, what it defines, and who uses those symbols.
4. **Before a refactor / signature change**, call `graph_blast_radius("path")` —
   the transitive set of files that may be affected.
5. **Use `graph_callchain("fn")`** to understand callers/callees before changing a
   function's behavior or contract.
6. **After structural changes** (new/renamed/moved files, new functions), call
   `graph_rebuild()` (or `node server.js rebuild`) so the graph stays accurate.

## Tools

| Tool | Use |
|------|-----|
| `graph_session_primer()` | Orientation. Call first, each session. |
| `graph_query(question)` | Find symbols/files by keyword(s). |
| `graph_files(subsystem)` | List a subsystem: boot, arch, mm, proc, ipc, syscall, exec, acpi, drivers, fs, kernel, user, tools, tests. |
| `graph_deps(file)` | includes / included-by / defines / used-by / calls-into. Before editing. |
| `graph_callchain(function)` | callers + callees. |
| `graph_blast_radius(file)` | transitive impact set. Before refactors. |
| `graph_rebuild()` | full re-index after structural changes. |

All outputs are compact summaries — the graph never dumps raw file contents.

## Setup

- **WSL / Linux / fresh clone:** `bash tools/graph_mcp/setup_graph.sh`
  (ensures Node ≥18, runs `npm ci`, builds the index, validates).
- **Windows host:** `cd tools/graph_mcp && npm ci && node server.js rebuild`.
- The MCP server is registered for the project in `/.mcp.json`; the Claude Code
  client will prompt once to trust it. If it isn't connected, the same queries
  work from the shell as a fallback (see CLI below).

## CLI (humans + CI)

```
node server.js primer
node server.js query "vmm map"
node server.js files mm
node server.js deps kernel/exec/elf.c
node server.js callchain vmm_map
node server.js blast kernel/mm/vmm.h
node server.js rebuild      # full re-index
node server.js selftest     # build + validate (used by CI)
```

## Design notes / limitations

- **NASM** is parsed by a focused extractor (`global`/`extern`/labels/`%macro`/
  `%define`/`incbin`/`%include`), not Tree-sitter: ASM grammars lack reliable
  prebuilt WASM and mature symbol rules, and NASM's syntax is regular enough to
  parse directly. This is what asm↔C linkage actually needs (e.g. asm
  `extern syscall_dispatch` links to the C definition).
- Cross-file usage is name-resolved (a symbol name referenced in file B that is
  defined in file A creates a usage edge). Same-name collisions across files are
  possible but rare in this codebase; treat `blast_radius` as a superset.
- The DB is generated and **git-ignored**; it is fully reproducible from source
  via `rebuild`. Dependencies are pinned in `package-lock.json`.
