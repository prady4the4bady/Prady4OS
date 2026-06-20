#!/usr/bin/env bash
# tools/graph_mcp/setup_graph.sh — one-command install + initial index build for
# the PRADYOS code graph. Idempotent; safe to re-run. Targets Linux/WSL (the
# documented build environment); on Windows run `npm ci` + `node server.js
# rebuild` from PowerShell instead (the deps are pure JS/WASM, no native build).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

have_node() {
  command -v node >/dev/null 2>&1 || return 1
  local major
  major="$(node -p 'process.versions.node.split(".")[0]' 2>/dev/null || echo 0)"
  [ "${major:-0}" -ge 18 ]
}

if ! have_node; then
  echo "[graph] Node >=18 not found — installing Node 20 (NodeSource, sudo)..."
  if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update -y
    sudo apt-get install -y curl ca-certificates
    curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
    sudo apt-get install -y nodejs
  else
    echo "[graph] ERROR: install Node >=18 manually, then re-run this script." >&2
    exit 1
  fi
fi

echo "[graph] node $(node -v), npm $(npm -v)"

if [ -f package-lock.json ]; then
  npm ci --no-audit --no-fund
else
  npm install --no-audit --no-fund
fi

echo "[graph] building initial index..."
node server.js rebuild

echo "[graph] validating..."
node server.js selftest

cat <<EOF

[graph] Setup complete.
  - MCP server:   node $DIR/server.js mcp   (registered via repo .mcp.json)
  - CLI examples: node $DIR/server.js primer
                  node $DIR/server.js query "vmm map"
                  node $DIR/server.js deps kernel/exec/elf.c
  See tools/graph_mcp/CLAUDE_GRAPH_USAGE.md for the mandatory session workflow.
EOF
