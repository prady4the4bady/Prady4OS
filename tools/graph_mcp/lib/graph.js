// lib/graph.js — sql.js (SQLite/WASM) store: schema, load/save, queries.
// All query helpers return compact strings (never raw file dumps).
import { createRequire } from 'node:module';
import { readFileSync, writeFileSync, existsSync, mkdirSync, renameSync } from 'node:fs';
import { dirname } from 'node:path';
import initSqlJs from 'sql.js';
import { DB_PATH, DB_DIR, SCHEMA_VERSION, SUBSYSTEM_ORDER } from './config.js';

const require = createRequire(import.meta.url);

let SQL = null;
async function sqlEngine() {
  if (!SQL) {
    const wasm = require.resolve('sql.js/dist/sql-wasm.wasm');
    SQL = await initSqlJs({ locateFile: () => wasm });
  }
  return SQL;
}

const SCHEMA = `
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE files(
  id INTEGER PRIMARY KEY, path TEXT UNIQUE, lang TEXT, subsystem TEXT,
  hash TEXT, mtime INTEGER, loc INTEGER);
CREATE TABLE symbols(
  id INTEGER PRIMARY KEY, file_id INTEGER, kind TEXT, name TEXT,
  line INTEGER, signature TEXT);
CREATE TABLE refs_raw(
  file_id INTEGER, src_fn TEXT, name TEXT, kind TEXT, line INTEGER);
CREATE INDEX idx_sym_name ON symbols(name);
CREATE INDEX idx_sym_file ON symbols(file_id);
CREATE INDEX idx_ref_name ON refs_raw(name);
CREATE INDEX idx_ref_file ON refs_raw(file_id);
CREATE INDEX idx_ref_kind ON refs_raw(kind);
`;

export async function openDb() {
  const engine = await sqlEngine();
  let db;
  if (existsSync(DB_PATH)) {
    db = new engine.Database(readFileSync(DB_PATH));
    const v = getMeta(db, 'schema_version');
    if (String(v) !== String(SCHEMA_VERSION)) {   // stale schema -> fresh
      db.close();
      db = freshDb(engine);
    }
  } else {
    db = freshDb(engine);
  }
  return db;
}

function freshDb(engine) {
  const db = new engine.Database();
  db.run(SCHEMA);
  setMeta(db, 'schema_version', String(SCHEMA_VERSION));
  return db;
}

export function saveDb(db) {
  if (!existsSync(DB_DIR)) mkdirSync(DB_DIR, { recursive: true });
  const bytes = Buffer.from(db.export());
  const tmp = DB_PATH + '.tmp';
  writeFileSync(tmp, bytes);
  renameSync(tmp, DB_PATH);             // atomic replace
}

// --- low-level helpers ------------------------------------------------------
export function rows(db, sql, params = []) {
  const out = [];
  const st = db.prepare(sql);
  try {
    st.bind(params);
    while (st.step()) out.push(st.getAsObject());
  } finally {
    st.free();
  }
  return out;
}
export function one(db, sql, params = []) {
  const r = rows(db, sql, params);
  return r.length ? r[0] : null;
}
export function getMeta(db, key) {
  const r = one(db, 'SELECT value FROM meta WHERE key=?', [key]);
  return r ? r.value : null;
}
export function setMeta(db, key, value) {
  db.run('INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value',
    [key, String(value)]);
}

// Resolve a user-supplied file string to a single files row (exact path, then
// suffix, then basename). Returns {row} or {candidates:[...]} or {none:true}.
export function resolveFile(db, q) {
  const norm = q.replace(/\\/g, '/').replace(/^\.?\//, '');
  let r = one(db, 'SELECT * FROM files WHERE path=?', [norm]);
  if (r) return { row: r };
  let cands = rows(db, 'SELECT * FROM files WHERE path LIKE ?', ['%/' + norm]);
  if (cands.length === 1) return { row: cands[0] };
  if (cands.length > 1) return { candidates: cands };
  const base = norm.split('/').pop();
  cands = rows(db, 'SELECT * FROM files WHERE path LIKE ? OR path=?', ['%/' + base, base]);
  if (cands.length === 1) return { row: cands[0] };
  if (cands.length > 1) return { candidates: cands };
  return { none: true };
}

function basename(p) { return p.split('/').pop(); }

// Files that #include the given file (by header basename match).
function includersOf(db, file) {
  const base = basename(file.path);
  return rows(db,
    `SELECT DISTINCT f.id, f.path, f.subsystem FROM refs_raw r JOIN files f ON f.id=r.file_id
     WHERE r.kind='include' AND (r.name=? OR r.name LIKE ?)`,
    [base, '%/' + base]);
}
// Files referencing a symbol defined in fileId (excluding itself).
function symbolUsersOf(db, fileId) {
  return rows(db,
    `SELECT DISTINCT f.id, f.path, f.subsystem
       FROM refs_raw r JOIN files f ON f.id=r.file_id
      WHERE r.kind IN ('call','ref','asm_extern')
        AND r.file_id<>?
        AND r.name IN (SELECT name FROM symbols WHERE file_id=?)`,
    [fileId, fileId]);
}

// --- rendered queries -------------------------------------------------------

export function primerText(db) {
  const nf = one(db, 'SELECT COUNT(*) c FROM files').c;
  const ns = one(db, 'SELECT COUNT(*) c FROM symbols').c;
  const built = getMeta(db, 'built_at') || '-';
  const commit = getMeta(db, 'commit') || '-';
  const summary = getMeta(db, 'summary') || '';
  const gates = getMeta(db, 'gates') || '';

  const L = [];
  L.push('# PRADYOS code graph — session primer');
  L.push(`repo graph: ${nf} files, ${ns} symbols | built ${built} | HEAD ${commit}`);
  if (summary) L.push(`status: ${summary}`);
  L.push('');
  L.push('## Subsystems (files · LOC · fns · structs · macros)');
  const sub = rows(db,
    `SELECT subsystem, COUNT(DISTINCT f.id) files, COALESCE(SUM(f.loc),0) loc
       FROM files f GROUP BY subsystem`);
  const byName = Object.fromEntries(sub.map(s => [s.subsystem, s]));
  for (const name of SUBSYSTEM_ORDER) {
    const s = byName[name];
    if (!s) continue;
    const fns = one(db, `SELECT COUNT(*) c FROM symbols s JOIN files f ON f.id=s.file_id WHERE f.subsystem=? AND s.kind='function'`, [name]).c;
    const st = one(db, `SELECT COUNT(*) c FROM symbols s JOIN files f ON f.id=s.file_id WHERE f.subsystem=? AND s.kind IN ('struct','union','enum')`, [name]).c;
    const mc = one(db, `SELECT COUNT(*) c FROM symbols s JOIN files f ON f.id=s.file_id WHERE f.subsystem=? AND s.kind='macro'`, [name]).c;
    L.push(`- ${name.padEnd(8)} ${String(s.files).padStart(3)}f ${String(s.loc).padStart(6)}L  ${fns} fn, ${st} types, ${mc} macros`);
  }
  L.push('');
  L.push('## Entry points / key functions');
  const want = ['kmain', '_start', 'elf_load', 'vmm_init', 'vmm_map', 'vmm_new_address_space',
    'sched_init', 'schedule', 'sched_create_user', 'syscall_init', 'syscall_dispatch',
    'vfs_mount', 'sfs_format', 'isr_dispatch'];
  for (const w of want) {
    const r = one(db, `SELECT s.name, f.path, s.line FROM symbols s JOIN files f ON f.id=s.file_id WHERE s.name=? AND s.kind='function' LIMIT 1`, [w]);
    if (r) L.push(`- ${r.name} — ${r.path}:${r.line}`);
  }
  L.push('');
  L.push('## Most-referenced functions (fan-in)');
  const hot = rows(db,
    `SELECT r.name, COUNT(*) c FROM refs_raw r WHERE r.kind='call'
       AND r.name IN (SELECT name FROM symbols WHERE kind='function')
     GROUP BY r.name ORDER BY c DESC LIMIT 8`);
  for (const h of hot) L.push(`- ${h.name} (${h.c} calls)`);
  if (gates) { L.push(''); L.push(`## Gates: ${gates}`); }
  L.push('');
  L.push('## How to use this graph (do this BEFORE opening files)');
  L.push('- graph_query("term") to locate symbols/files; graph_files("subsystem") to list a subsystem.');
  L.push('- graph_deps("file") BEFORE editing a file; graph_blast_radius("file") BEFORE a refactor.');
  L.push('- graph_callchain("fn") for callers/callees; graph_rebuild() after structural changes.');
  return L.join('\n');
}

export function queryText(db, question) {
  const terms = String(question || '').toLowerCase().split(/[^a-z0-9_]+/i).filter(t => t.length >= 2);
  if (!terms.length) return 'query: provide at least one search term (>=2 chars).';
  const like = terms.map(() => '(LOWER(s.name) LIKE ?)').join(' OR ');
  const params = terms.map(t => '%' + t + '%');
  const syms = rows(db,
    `SELECT s.kind, s.name, s.line, s.signature, f.path FROM symbols s JOIN files f ON f.id=s.file_id
      WHERE ${like} ORDER BY (CASE s.kind WHEN 'function' THEN 0 WHEN 'struct' THEN 1 ELSE 2 END), s.name LIMIT 30`,
    params);
  const fparams = terms.map(t => '%' + t + '%');
  const flike = terms.map(() => 'LOWER(path) LIKE ?').join(' OR ');
  const files = rows(db, `SELECT path, subsystem, loc FROM files WHERE ${flike} ORDER BY path LIMIT 12`, fparams);

  const L = [`# query: ${terms.join(' ')}`];
  L.push(`symbols (${syms.length}):`);
  for (const s of syms) {
    const sig = s.signature ? '  ' + s.signature.slice(0, 90) : '';
    L.push(`  ${s.kind.padEnd(9)} ${s.name} — ${s.path}:${s.line}${sig}`);
  }
  if (files.length) {
    L.push(`files (${files.length}):`);
    for (const f of files) L.push(`  ${f.path} [${f.subsystem}, ${f.loc}L]`);
  }
  if (!syms.length && !files.length) L.push('  (no matches)');
  return L.join('\n');
}

export function filesText(db, subsystem) {
  const sub = String(subsystem || '').trim().toLowerCase();
  const fs = rows(db,
    `SELECT f.id, f.path, f.lang, f.loc,
            (SELECT COUNT(*) FROM symbols s WHERE s.file_id=f.id) syms
       FROM files f WHERE f.subsystem=? ORDER BY f.path`, [sub]);
  if (!fs.length) {
    const all = rows(db, 'SELECT DISTINCT subsystem FROM files ORDER BY subsystem').map(r => r.subsystem);
    return `files: no subsystem "${sub}". Known: ${all.join(', ')}`;
  }
  const L = [`# subsystem ${sub} — ${fs.length} files`];
  for (const f of fs) {
    const top = rows(db, `SELECT name FROM symbols WHERE file_id=? AND kind='function' ORDER BY line LIMIT 4`, [f.id])
      .map(r => r.name).join(', ');
    L.push(`- ${f.path} [${f.lang}, ${f.loc}L, ${f.syms} sym]${top ? '  fns: ' + top : ''}`);
  }
  return L.join('\n');
}

export function depsText(db, fileQ) {
  const res = resolveFile(db, fileQ);
  if (res.none) return `deps: no file matching "${fileQ}".`;
  if (res.candidates) return `deps: ambiguous "${fileQ}":\n` + res.candidates.map(c => '  ' + c.path).join('\n');
  const f = res.row;
  const L = [`# deps ${f.path} [${f.subsystem}, ${f.lang}, ${f.loc}L]`];

  const inc = rows(db, `SELECT DISTINCT name FROM refs_raw WHERE file_id=? AND kind IN ('include','incbin') ORDER BY name`, [f.id]);
  L.push(`includes (${inc.length}): ${inc.map(i => i.name).join(', ') || '-'}`);

  const incBy = includersOf(db, f);
  L.push(`included-by (${incBy.length}): ${incBy.map(i => i.path).join(', ') || '-'}`);

  const defs = rows(db, `SELECT kind, COUNT(*) c FROM symbols WHERE file_id=? GROUP BY kind`, [f.id]);
  L.push(`defines: ${defs.map(d => d.c + ' ' + d.kind).join(', ') || '-'}`);

  const usedBy = symbolUsersOf(db, f.id);
  L.push(`symbols used-by (${usedBy.length}): ${usedBy.map(u => u.path).join(', ') || '-'}`);

  const callsOut = rows(db,
    `SELECT DISTINCT df.path FROM refs_raw r JOIN symbols s ON s.name=r.name JOIN files df ON df.id=s.file_id
      WHERE r.file_id=? AND r.kind='call' AND s.kind='function' AND df.id<>? ORDER BY df.path`, [f.id, f.id]);
  L.push(`calls into (${callsOut.length}): ${callsOut.map(c => c.path).join(', ') || '-'}`);
  return L.join('\n');
}

export function callchainText(db, fnName) {
  const fn = String(fnName || '').trim();
  const defs = rows(db, `SELECT s.name, f.path, s.line, s.signature FROM symbols s JOIN files f ON f.id=s.file_id
                          WHERE s.name=? AND s.kind='function'`, [fn]);
  const L = [`# callchain ${fn}`];
  if (defs.length) {
    for (const d of defs) L.push(`def: ${d.path}:${d.line}  ${d.signature ? d.signature.slice(0, 80) : ''}`);
  } else {
    L.push('def: (not found as a function symbol)');
  }
  const callees = rows(db, `SELECT DISTINCT name FROM refs_raw WHERE src_fn=? AND kind='call' ORDER BY name`, [fn]);
  L.push(`calls (${callees.length}): ${callees.map(c => c.name).join(', ') || '-'}`);
  const callers = rows(db,
    `SELECT DISTINCT r.src_fn, f.path FROM refs_raw r JOIN files f ON f.id=r.file_id
      WHERE r.name=? AND r.kind='call' AND r.src_fn IS NOT NULL ORDER BY r.src_fn`, [fn]);
  L.push(`called-by (${callers.length}): ${callers.map(c => `${c.src_fn} (${basename(c.path)})`).join(', ') || '-'}`);
  return L.join('\n');
}

export function blastText(db, fileQ) {
  const res = resolveFile(db, fileQ);
  if (res.none) return `blast_radius: no file matching "${fileQ}".`;
  if (res.candidates) return `blast_radius: ambiguous "${fileQ}":\n` + res.candidates.map(c => '  ' + c.path).join('\n');
  const start = res.row;
  const seen = new Map([[start.id, start]]);
  let frontier = [start];
  let depth = 0;
  const MAXD = 6;
  while (frontier.length && depth < MAXD) {
    const next = [];
    for (const f of frontier) {
      for (const u of [...includersOf(db, f), ...symbolUsersOf(db, f.id)]) {
        if (!seen.has(u.id)) { seen.set(u.id, u); next.push(u); }
      }
    }
    frontier = next;
    depth++;
  }
  seen.delete(start.id);
  const affected = [...seen.values()];
  const L = [`# blast_radius ${start.path}`];
  L.push(`${affected.length} files may be affected (transitive include + symbol users, depth<=${MAXD}):`);
  const byss = {};
  for (const a of affected) (byss[a.subsystem] ||= []).push(basename(a.path));
  for (const name of SUBSYSTEM_ORDER) {
    if (!byss[name]) continue;
    L.push(`  ${name}: ${byss[name].sort().join(', ')}`);
  }
  if (!affected.length) L.push('  (none — leaf file)');
  return L.join('\n');
}
