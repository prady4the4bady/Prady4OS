// lib/indexer.js — discover sources, parse them (C/Rust via Tree-sitter, NASM via
// a focused symbol extractor), and populate the SQLite graph. Hash-based
// incremental: only changed files are re-parsed.
import { createRequire } from 'node:module';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative, extname } from 'node:path';
import { createHash } from 'node:crypto';
import { execSync } from 'node:child_process';
import Parser from 'web-tree-sitter';
import { REPO_ROOT, LANG_BY_EXT, IGNORE_DIRS, subsystemOf } from './config.js';
import { rows, one, setMeta } from './graph.js';

const require = createRequire(import.meta.url);

let PARSERS = null;
async function getParsers() {
  if (PARSERS) return PARSERS;
  await Parser.init({ locateFile: () => require.resolve('web-tree-sitter/tree-sitter.wasm') });
  const gdir = join(require.resolve('tree-sitter-wasms/package.json'), '..', 'out');
  const load = async (file) => {
    const lang = await Parser.Language.load(join(gdir, file));
    const p = new Parser();
    p.setLanguage(lang);
    return p;
  };
  PARSERS = { c: await load('tree-sitter-c.wasm'), rust: await load('tree-sitter-rust.wasm') };
  return PARSERS;
}

// --- file discovery ---------------------------------------------------------
function discover() {
  const out = [];
  (function walk(dir) {
    for (const ent of readdirSync(dir, { withFileTypes: true })) {
      if (ent.isDirectory()) {
        if (IGNORE_DIRS.has(ent.name)) continue;
        walk(join(dir, ent.name));
      } else if (ent.isFile()) {
        const lang = LANG_BY_EXT[extname(ent.name)];
        if (!lang) continue;
        const abs = join(dir, ent.name);
        const rel = relative(REPO_ROOT, abs).replace(/\\/g, '/');
        out.push({ abs, rel, lang });
      }
    }
  })(REPO_ROOT);
  return out;
}

const sha1 = (s) => createHash('sha1').update(s).digest('hex');
const txt = (src, n) => src.slice(n.startIndex, n.endIndex);
const collapse = (s) => s.replace(/\s+/g, ' ').trim();

// --- C / Rust shared helpers -----------------------------------------------
function funcName(node) {                  // descend declarators to the name id
  if (!node) return null;
  if (node.type === 'function_declarator') {
    return node.childForFieldName('declarator');
  }
  for (let i = 0; i < node.childCount; i++) {
    const r = funcName(node.child(i));
    if (r) return r;
  }
  return null;
}
function innerName(decl) {                  // declarator -> innermost identifier
  let n = decl;
  while (n) {
    if (n.type === 'identifier' || n.type === 'type_identifier' || n.type === 'field_identifier') return n;
    const d = n.childForFieldName('declarator') || n.childForFieldName('name');
    if (!d) break;
    n = d;
  }
  return null;
}

function parseC(src, parser) {
  const tree = parser.parse(src);
  const symbols = [];
  const refs = [];
  const seenRef = new Set();
  const addRef = (kind, name, srcFn, line) => {
    if (!name) return;
    const k = `${kind}|${srcFn || ''}|${name}`;
    if (seenRef.has(k)) return;
    seenRef.add(k);
    refs.push({ kind, name, src_fn: srcFn || null, line });
  };
  const lineOf = (n) => n.startPosition.row + 1;

  function visit(node, fn) {
    switch (node.type) {
      case 'function_definition': {
        const body = node.childForFieldName('body');
        const nm = funcName(node.childForFieldName('declarator'));
        const name = nm ? txt(src, nm) : null;
        if (name) {
          const sig = collapse(src.slice(node.startIndex, body ? body.startIndex : node.endIndex)).slice(0, 120);
          symbols.push({ kind: 'function', name, line: lineOf(node), signature: sig });
        }
        for (let i = 0; i < node.childCount; i++) visit(node.child(i), name || fn);
        return;
      }
      case 'struct_specifier':
      case 'union_specifier':
      case 'enum_specifier': {
        const nm = node.childForFieldName('name');
        const body = node.childForFieldName('body');
        if (nm && body) {                       // a DEFINITION (has a body), not `struct S *p`
          const kind = node.type.replace('_specifier', '');
          symbols.push({ kind, name: txt(src, nm), line: lineOf(node), signature: kind + ' ' + txt(src, nm) });
        }
        break;
      }
      case 'type_definition': {
        const d = node.childForFieldName('declarator');
        const id = innerName(d);
        if (id) symbols.push({ kind: 'typedef', name: txt(src, id), line: lineOf(node), signature: collapse(txt(src, node)).slice(0, 120) });
        break;
      }
      case 'preproc_def':
      case 'preproc_function_def': {
        const nm = node.childForFieldName('name');
        if (nm) symbols.push({ kind: 'macro', name: txt(src, nm), line: lineOf(node), signature: txt(src, nm) });
        break;
      }
      case 'preproc_include': {
        const p = node.childForFieldName('path');
        if (p) {
          const raw = txt(src, p).replace(/^[<"]|[">]$/g, '');
          addRef('include', raw, null, lineOf(node));
        }
        break;
      }
      case 'declaration': {
        // top-level (function prototype or global/extern variable)
        if (node.parent && node.parent.type === 'translation_unit') {
          const isExtern = /^\s*extern\b/.test(txt(src, node));
          let isProto = false;
          for (let i = 0; i < node.childCount; i++) {
            const fd = funcName(node.child(i));
            if (fd && node.child(i).type !== 'identifier') { isProto = true; break; }
          }
          if (isProto) {
            const fnId = funcName(node);
            if (fnId) symbols.push({ kind: 'prototype', name: txt(src, fnId), line: lineOf(node), signature: collapse(txt(src, node)).slice(0, 120) });
          } else {
            for (let i = 0; i < node.childCount; i++) {
              const c = node.child(i);
              if (c.type === 'init_declarator' || c.type === 'declarator' ||
                  c.type === 'array_declarator' || c.type === 'pointer_declarator' || c.type === 'identifier') {
                const id = innerName(c.type === 'init_declarator' ? c.childForFieldName('declarator') : c);
                if (id) symbols.push({ kind: isExtern ? 'extern' : 'global', name: txt(src, id), line: lineOf(node), signature: collapse(txt(src, node)).slice(0, 100) });
              }
            }
          }
        }
        break;
      }
      case 'call_expression': {
        const callee = node.childForFieldName('function');
        if (callee && callee.type === 'identifier') addRef('call', txt(src, callee), fn, lineOf(node));
        break;
      }
      case 'type_identifier':
        addRef('ref', txt(src, node), fn, lineOf(node));
        break;
      case 'identifier': {
        const t = txt(src, node);
        if (/^[A-Z][A-Z0-9_]{2,}$/.test(t)) addRef('ref', t, fn, lineOf(node));
        break;
      }
    }
    for (let i = 0; i < node.childCount; i++) visit(node.child(i), fn);
  }
  visit(tree.rootNode, null);
  tree.delete();
  return { symbols, refs };
}

function parseRust(src, parser) {
  const tree = parser.parse(src);
  const symbols = [];
  const refs = [];
  const lineOf = (n) => n.startPosition.row + 1;
  function visit(node, fn) {
    switch (node.type) {
      case 'function_item': {
        const nm = node.childForFieldName('name');
        const name = nm ? txt(src, nm) : null;
        if (name) {
          const body = node.childForFieldName('body');
          symbols.push({ kind: 'function', name, line: lineOf(node), signature: collapse(src.slice(node.startIndex, body ? body.startIndex : node.endIndex)).slice(0, 120) });
          for (let i = 0; i < node.childCount; i++) visit(node.child(i), name);
          return;
        }
        break;
      }
      case 'struct_item': case 'enum_item': case 'union_item': {
        const nm = node.childForFieldName('name');
        if (nm) symbols.push({ kind: node.type.replace('_item', ''), name: txt(src, nm), line: lineOf(node), signature: node.type.replace('_item', '') + ' ' + txt(src, nm) });
        break;
      }
      case 'const_item': case 'static_item': {
        const nm = node.childForFieldName('name');
        if (nm) symbols.push({ kind: 'global', name: txt(src, nm), line: lineOf(node), signature: collapse(txt(src, node)).slice(0, 100) });
        break;
      }
      case 'call_expression': {
        const f = node.childForFieldName('function');
        if (f && (f.type === 'identifier')) refs.push({ kind: 'call', name: txt(src, f), src_fn: fn || null, line: lineOf(node) });
        break;
      }
    }
    for (let i = 0; i < node.childCount; i++) visit(node.child(i), fn);
  }
  visit(tree.rootNode, null);
  tree.delete();
  return { symbols, refs };
}

// --- NASM symbol extractor (focused; tree-sitter ASM grammars lack reliable
// prebuilds and mature symbol rules — NASM's syntax is regular enough to parse
// directly, which is what the cross-language linkage actually needs). ---------
function parseAsm(src) {
  const symbols = [];
  const refs = [];
  const exported = new Set();
  const lines = src.split(/\r?\n/);
  // pass 1: exports / externs / includes / macros
  lines.forEach((raw, i) => {
    const line = raw.split(';')[0];
    let m;
    if ((m = /^\s*global\s+([A-Za-z_.][\w.]*)/.exec(line))) exported.add(m[1]);
    if ((m = /^\s*extern\s+([A-Za-z_.][\w.]*)/.exec(line))) refs.push({ kind: 'asm_extern', name: m[1], src_fn: null, line: i + 1 });
    if ((m = /^\s*incbin\s+["']([^"']+)["']/.exec(line))) refs.push({ kind: 'incbin', name: m[1], src_fn: null, line: i + 1 });
    if ((m = /^\s*%include\s+["']([^"']+)["']/.exec(line))) refs.push({ kind: 'include', name: m[1], src_fn: null, line: i + 1 });
    if ((m = /^\s*%(?:define|macro)\s+([A-Za-z_]\w*)/.exec(line))) symbols.push({ kind: 'macro', name: m[1], line: i + 1, signature: collapse(line) });
  });
  // pass 2: top-level labels (not local ".foo")
  lines.forEach((raw, i) => {
    const line = raw.split(';')[0];
    const m = /^([A-Za-z_][\w]*):/.exec(line);
    if (m) {
      const name = m[1];
      const kind = exported.has(name) ? 'function' : 'label';
      symbols.push({ kind, name, line: i + 1, signature: kind === 'function' ? `asm global ${name}` : `asm ${name}` });
    }
  });
  // exported symbols with no matching label (data blobs declared elsewhere)
  for (const e of exported) {
    if (!symbols.some((s) => s.name === e)) symbols.push({ kind: 'global', name: e, line: 0, signature: `asm global ${e}` });
  }
  return { symbols, refs };
}

// --- repo facts for the primer ---------------------------------------------
function repoFacts(db) {
  setMeta(db, 'built_at', new Date().toISOString().replace('T', ' ').slice(0, 19) + 'Z');
  try {
    const c = execSync('git rev-parse --short HEAD', { cwd: REPO_ROOT, stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim();
    if (c) setMeta(db, 'commit', c);
  } catch { /* git optional */ }
  try {
    const mk = readFileSync(join(REPO_ROOT, 'Makefile'), 'utf8');
    const gates = [...mk.matchAll(/^(smoke[\w-]*):/gm)].map((m) => m[1]);
    if (gates.length) setMeta(db, 'gates', [...new Set(gates)].join(' '));
  } catch { /* */ }
  try {
    const bs = readFileSync(join(REPO_ROOT, 'docs', 'build_status.md'), 'utf8');
    const m = /\*\*Current phase:\*\*\s*([^\n.]+)/.exec(bs);
    if (m) setMeta(db, 'summary', collapse(m[1]).slice(0, 160));
  } catch { /* */ }
}

// Index the repo. full=true forces a complete re-parse; otherwise only files
// whose content hash changed are re-parsed.
export async function index(db, { full = false } = {}) {
  const parsers = await getParsers();
  repoFacts(db);
  const found = discover();
  const foundPaths = new Set(found.map((f) => f.rel));
  const existing = Object.fromEntries(rows(db, 'SELECT id, path, hash FROM files').map((r) => [r.path, r]));

  const stats = { scanned: found.length, parsed: 0, skipped: 0, removed: 0, symbols: 0, refs: 0 };

  // remove files that disappeared from disk
  for (const path of Object.keys(existing)) {
    if (!foundPaths.has(path)) {
      const id = existing[path].id;
      db.run('DELETE FROM symbols WHERE file_id=?', [id]);
      db.run('DELETE FROM refs_raw WHERE file_id=?', [id]);
      db.run('DELETE FROM files WHERE id=?', [id]);
      stats.removed++;
    }
  }

  for (const f of found) {
    const src = readFileSync(f.abs, 'utf8');
    const hash = sha1(src);
    const prev = existing[f.rel];
    if (!full && prev && prev.hash === hash) { stats.skipped++; continue; }

    if (prev) {
      db.run('DELETE FROM symbols WHERE file_id=?', [prev.id]);
      db.run('DELETE FROM refs_raw WHERE file_id=?', [prev.id]);
      db.run('DELETE FROM files WHERE id=?', [prev.id]);
    }
    const loc = src.length ? src.split('\n').length : 0;
    const mtime = Math.floor(statSync(f.abs).mtimeMs);
    db.run('INSERT INTO files(path,lang,subsystem,hash,mtime,loc) VALUES(?,?,?,?,?,?)',
      [f.rel, f.lang, subsystemOf(f.rel), hash, mtime, loc]);
    const fileId = one(db, 'SELECT id FROM files WHERE path=?', [f.rel]).id;

    const parsed = f.lang === 'c' ? parseC(src, parsers.c)
      : f.lang === 'rust' ? parseRust(src, parsers.rust)
      : parseAsm(src);
    for (const s of parsed.symbols) {
      db.run('INSERT INTO symbols(file_id,kind,name,line,signature) VALUES(?,?,?,?,?)',
        [fileId, s.kind, s.name, s.line, s.signature || null]);
      stats.symbols++;
    }
    for (const r of parsed.refs) {
      db.run('INSERT INTO refs_raw(file_id,src_fn,name,kind,line) VALUES(?,?,?,?,?)',
        [fileId, r.src_fn || null, r.name, r.kind, r.line || 0]);
      stats.refs++;
    }
    stats.parsed++;
  }
  setMeta(db, 'last_index', new Date().toISOString());
  return stats;
}
