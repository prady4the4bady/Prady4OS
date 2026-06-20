// lib/config.js — paths, language map, subsystem map, ignore rules.
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));      // tools/graph_mcp/lib
export const TOOL_DIR = resolve(here, '..');               // tools/graph_mcp
export const REPO_ROOT = resolve(TOOL_DIR, '..', '..');    // repo root
export const DB_DIR = resolve(TOOL_DIR, '.graph');
export const DB_PATH = resolve(DB_DIR, 'graph.sqlite');
export const SCHEMA_VERSION = 1;

// Source extensions we index, mapped to a language tag.
export const LANG_BY_EXT = {
  '.c': 'c',
  '.h': 'c',
  '.asm': 'asm',
  '.inc': 'asm',
  '.s': 'asm',
  '.S': 'asm',
  '.rs': 'rust',
};

// Directories never walked (build output, VCS, deps, generated graph).
export const IGNORE_DIRS = new Set([
  '.git', 'node_modules', 'build', 'dist', 'target', '.graph', '.github',
]);

// Map a repo-relative (forward-slash) path to a subsystem bucket. Order matters:
// the first prefix that matches wins.
const SUBSYSTEM_RULES = [
  ['boot/', 'boot'],
  ['arch/', 'arch'],
  ['kernel/mm/', 'mm'],
  ['kernel/proc/', 'proc'],
  ['kernel/ipc/', 'ipc'],
  ['kernel/syscall/', 'syscall'],
  ['kernel/exec/', 'exec'],
  ['kernel/acpi/', 'acpi'],
  ['kernel/drivers/', 'drivers'],
  ['kernel/fs/', 'fs'],
  ['kernel/', 'kernel'],     // remaining top-level kernel/*.c|h
  ['user/', 'user'],
  ['tools/', 'tools'],
  ['tests/', 'tests'],
];

export function subsystemOf(relPath) {
  for (const [prefix, name] of SUBSYSTEM_RULES) {
    if (relPath.startsWith(prefix)) return name;
  }
  return 'other';
}

// The canonical subsystem ordering used in the primer / grouped output.
export const SUBSYSTEM_ORDER = [
  'boot', 'arch', 'mm', 'proc', 'ipc', 'syscall', 'exec',
  'acpi', 'drivers', 'fs', 'kernel', 'user', 'tools', 'tests', 'other',
];
