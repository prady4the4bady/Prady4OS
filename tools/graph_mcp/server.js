#!/usr/bin/env node
// tools/graph_mcp/server.js — PRADYOS code knowledge graph.
//
// Two faces, one codebase:
//   * MCP server  (default / `mcp`)  — exposes graph_* tools to a Claude session.
//   * CLI         (`rebuild`,`selftest`,`primer`,`query`,`files`,`deps`,
//                  `callchain`,`blast`) — for setup, CI validation, and humans.
//
// Stack is pure JS/WASM (sql.js + web-tree-sitter + tree-sitter-wasms), so one
// node_modules works on the Windows host, WSL, and Linux CI with no native build.
import { openDb, saveDb, one,
  primerText, queryText, filesText, depsText, callchainText, blastText } from './lib/graph.js';
import { index } from './lib/indexer.js';

const log = (...a) => console.error('[graph]', ...a);

async function ready({ full = false } = {}) {
  const db = await openDb();
  const empty = !one(db, 'SELECT 1 FROM files LIMIT 1');
  const stats = await index(db, { full: full || empty });
  saveDb(db);
  return { db, stats };
}

// --- MCP tool definitions (JSON Schema; no zod dependency) -------------------
const TOOLS = [
  { name: 'graph_session_primer', description: 'Compact orientation for PRADYOS: subsystems, key entry points, gates. Call FIRST each session, before opening source files.',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false } },
  { name: 'graph_query', description: 'Search the code graph for symbols/files by keyword(s). Use to locate code instead of blind file reads.',
    inputSchema: { type: 'object', properties: { question: { type: 'string', description: 'search term(s)' } }, required: ['question'], additionalProperties: false } },
  { name: 'graph_files', description: 'List the files in a subsystem (boot, arch, mm, proc, ipc, syscall, exec, acpi, drivers, fs, kernel, user, tools, tests).',
    inputSchema: { type: 'object', properties: { subsystem: { type: 'string' } }, required: ['subsystem'], additionalProperties: false } },
  { name: 'graph_deps', description: 'Dependencies of a file: includes, included-by, defined symbols, who uses them, and files it calls into. Call BEFORE editing a file.',
    inputSchema: { type: 'object', properties: { file: { type: 'string' } }, required: ['file'], additionalProperties: false } },
  { name: 'graph_callchain', description: 'Callers and callees of a function.',
    inputSchema: { type: 'object', properties: { function: { type: 'string' } }, required: ['function'], additionalProperties: false } },
  { name: 'graph_blast_radius', description: 'Transitive set of files that may be affected if a file changes (reverse includes + symbol users). Call BEFORE a refactor.',
    inputSchema: { type: 'object', properties: { file: { type: 'string' } }, required: ['file'], additionalProperties: false } },
  { name: 'graph_rebuild', description: 'Full re-index of the code graph. Call after structural changes (new/renamed/moved files).',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false } },
];

async function runTool(db, name, args = {}) {
  switch (name) {
    case 'graph_session_primer': return primerText(db);
    case 'graph_query': return queryText(db, args.question);
    case 'graph_files': return filesText(db, args.subsystem);
    case 'graph_deps': return depsText(db, args.file);
    case 'graph_callchain': return callchainText(db, args.function);
    case 'graph_blast_radius': return blastText(db, args.file);
    case 'graph_rebuild': {
      const st = await index(db, { full: true });
      saveDb(db);
      return `rebuilt: ${JSON.stringify(st)}`;
    }
    default: return `unknown tool: ${name}`;
  }
}

async function serveMcp() {
  const { Server } = await import('@modelcontextprotocol/sdk/server/index.js');
  const { StdioServerTransport } = await import('@modelcontextprotocol/sdk/server/stdio.js');
  const { ListToolsRequestSchema, CallToolRequestSchema } = await import('@modelcontextprotocol/sdk/types.js');

  const t0 = Date.now();
  const { db, stats } = await ready();
  log(`indexed ${stats.parsed} parsed / ${stats.skipped} cached in ${Date.now() - t0}ms`);

  const server = new Server({ name: 'pradyos-graph', version: '0.1.0' }, { capabilities: { tools: {} } });
  server.setRequestHandler(ListToolsRequestSchema, async () => ({ tools: TOOLS }));
  server.setRequestHandler(CallToolRequestSchema, async (req) => {
    try {
      const text = await runTool(db, req.params.name, req.params.arguments || {});
      return { content: [{ type: 'text', text: String(text) }] };
    } catch (e) {
      return { content: [{ type: 'text', text: `graph error: ${e.message}` }], isError: true };
    }
  });
  await server.connect(new StdioServerTransport());
  log('MCP server ready on stdio');
}

async function cli(cmd, rest) {
  if (cmd === 'rebuild' || cmd === 'index') {
    const { stats } = await ready({ full: cmd === 'rebuild' });
    console.log(`graph ${cmd}: ${JSON.stringify(stats)}`);
    return 0;
  }
  if (cmd === 'selftest') return selftest();

  const { db } = await ready();
  const arg = rest.join(' ').trim();
  let out;
  switch (cmd) {
    case 'primer': out = primerText(db); break;
    case 'query': out = queryText(db, arg); break;
    case 'files': out = filesText(db, arg); break;
    case 'deps': out = depsText(db, arg); break;
    case 'callchain': out = callchainText(db, arg); break;
    case 'blast': case 'blast_radius': out = blastText(db, arg); break;
    default:
      console.error(`usage: server.js [mcp|rebuild|selftest|primer|query <t>|files <ss>|deps <f>|callchain <fn>|blast <f>]`);
      return 2;
  }
  console.log(out);
  return 0;
}

async function selftest() {
  const t0 = Date.now();
  const { db, stats } = await ready({ full: true });
  const checks = [];
  const ok = (name, cond, detail = '') => { checks.push({ name, pass: !!cond, detail }); };

  ok('files indexed', stats.symbols > 0 && one(db, 'SELECT COUNT(*) c FROM files').c > 10,
    `${one(db, 'SELECT COUNT(*) c FROM files').c} files, ${stats.symbols} symbols`);
  const primer = primerText(db);
  ok('primer non-empty', primer.length > 200);
  ok('primer under budget (<12000 chars ~3000 tok)', primer.length < 12000, `${primer.length} chars`);
  ok('query finds vmm', /vmm/i.test(queryText(db, 'vmm')));
  ok('files(mm) lists vmm.c', /vmm\.c/.test(filesText(db, 'mm')));
  ok('deps(vmm.c) resolves', /includes/.test(depsText(db, 'kernel/mm/vmm.c')));
  ok('callchain(vmm_map)', /callchain vmm_map/.test(callchainText(db, 'vmm_map')));
  ok('blast(vmm.h) non-empty', /blast_radius/.test(blastText(db, 'kernel/mm/vmm.h')));
  ok('asm<->C linkage (syscall_dispatch used-by asm)', /syscall_entry\.asm/.test(depsText(db, 'kernel/syscall/syscall.c')));

  let pass = 0;
  for (const c of checks) { console.log(`  [${c.pass ? 'PASS' : 'FAIL'}] ${c.name}${c.detail ? '  (' + c.detail + ')' : ''}`); if (c.pass) pass++; }
  const allOk = pass === checks.length;
  console.log(`GRAPH_SELFTEST: ${allOk ? 'OK' : 'FAIL'} (${pass}/${checks.length}, ${Date.now() - t0}ms)`);
  return allOk ? 0 : 1;
}

const [cmd, ...rest] = process.argv.slice(2);
if (!cmd || cmd === 'mcp') {
  serveMcp().catch((e) => { log('fatal:', e.stack || e.message); process.exit(1); });
} else {
  cli(cmd, rest).then((code) => process.exit(code)).catch((e) => { log('fatal:', e.stack || e.message); process.exit(1); });
}
