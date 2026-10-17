# DDR-1172: Dependabot triage, 2026-10 (graph_mcp security fixes)

Status: done in this PR. Operator item 2 of issue #45 comment 6200777349.

## 1. Scope measured

- The Dependabot alerts API refuses this session (`403 Resource not accessible
  by integration`), so the alert list is taken from `npm audit` in
  `tools/graph_mcp`, the only npm tree in the repo. Reported once on #45.
- `npm audit` on `d201c2c`: 3 vulnerable packages (1 critical, 2 moderate):
  `proxy-addr` 2.0.7 (GHSA-jqcg-44mw-7w3h, critical), `ip-address` 10.5.0
  (GHSA-rpw4-54j3-4h4q, GHSA-2vr4-cq9g-pvrc, GHSA-j6r3-76f7-8jcv,
  GHSA-h3mg-xc3c-68pw), `fast-uri` 3.1.7 (GHSA-hrr3-gc8f-f4qj).
- Blast radius: `tools/graph_mcp` is dev tooling (code-graph MCP server). It is
  not in `kernel.bin` or the ISO (`grep graph_mcp Makefile` is empty). One CI
  job runs `npm ci` + `node server.js selftest` there.

## 2. Change

`npm audit fix` (no `--force`): all three fixes are in-range, so
`package.json` is unchanged and the diff is `package-lock.json` only, with
exactly three version deltas and 97 -> 97 packages:

| package | before | after |
|---|---|---|
| fast-uri | 3.1.7 | 3.1.8 |
| ip-address | 10.5.0 | 10.7.4 |
| proxy-addr | 2.0.7 | 2.0.8 |

Measured after: `npm audit` = 0 vulnerabilities; `npm ci` rc=0;
`node server.js selftest` = `GRAPH_SELFTEST: OK (9/9)`.

## 3. Dependabot PR disposition

- #19 (fast-uri 3.1.8): superseded by this PR, closed after merge.
- #9 (sql.js patch), #33 (MCP SDK minor), #42 (web-tree-sitter 0.22 -> 0.27):
  not security fixes. Each is updated onto the tip and merged only if CI is
  green, with the selftest as the gate; a red one is closed with the reason.
- #7, #8, #30 (actions/checkout, setup-python, download-artifact majors): they
  change `ci.yml`. Same rule: rebase, merge on green.

## 4. Not claimed

No kernel change; `kernel.bin` unchanged. The 403 means GitHub-side alerts
outside `npm audit`'s view (if any) are not enumerated here.
