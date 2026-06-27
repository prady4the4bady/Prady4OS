# DDR-AETHER — detailed design record for Layer 6 (AETHER)

Companion to **ADR-026**. The ADR fixes the *decisions*; this DDR fixes the
*implementation* — exact files, struct layouts, syscall numbers, control flow,
and gate harnesses — so the build is mechanical and reviewable.

## 0. File plan

| File | Role |
|------|------|
| `kernel/aether/aether.h` | shared types: action entry, audit entry, enums, mode flag, public fns |
| `kernel/aether/aether_queue.c` | 256-entry action queue; submit/poll/approve/reject + expiry |
| `kernel/aether/aether_audit.c` | 4096-entry circular audit log + wrap event |
| `kernel/aether/aether_mem.c` | per-process mem cap charge/uncharge + OOM kill; `SYS_SET_MEM_LIMIT` |
| `kernel/syscall/sys_aether.c` | the 10 NSI handlers; registered from `syscall_init` |
| `user/aether_daemon.c` | PID-2 daemon (musl); config read, IPC surface, test-mode auto-spawn |
| `user/agent_base.c` | agent template (musl); test/live response → submit/poll/execute |
| `docs/decisions/ADR-026-*.md`, `DDR-AETHER.md` | this design |

Rate limiting + mode flag live in `kernel/syscall/syscall.c` (dispatcher) and
`kernel/aether/aether.c`-owned globals to avoid new flat files in `kernel/` root
(CLAUDE.md §3). `kernel/aether/` is the new subsystem directory.

## 1. Syscall numbers (NSI, append-only after 28)

```
#define SYS_GET_MODE       29  /* () -> g_sovereign_mode (0|1)                       */
#define SYS_SET_MODE       30  /* (mode) -> 0 | -EPERM   (needs CAP_SOVEREIGN)       */
#define SYS_SUBMIT_ACTION  31  /* (type, payload*, len) -> action_id | -EAGAIN|-EPERM*/
#define SYS_POLL_RESULT    32  /* (action_id) -> status enum | -ESRCH                */
#define SYS_APPROVE_ACTION 33  /* (action_id) -> 0 | -EPERM|-ESRCH (CAP_SOVEREIGN)   */
#define SYS_REJECT_ACTION  34  /* (action_id) -> 0 | -EPERM|-ESRCH (CAP_SOVEREIGN)   */
#define SYS_SPAWN_AGENT    35  /* (path*, task*) -> child pid | -EPERM  (CAP_AGENT)  */
#define SYS_KILL_AGENT     36  /* (pid) -> 0 | -EPERM|-ESRCH       (CAP_AGENT)       */
#define SYS_READ_AUDIT     37  /* (buf*, max_entries) -> n copied                    */
#define SYS_SET_MEM_LIMIT  38  /* (pid, bytes) -> 0 | -EPERM (lower-only / own/child)*/
```

`MAX_SYSCALLS` stays 64 (38 ≤ 63). Each is registered in `syscall_init` via a new
`sys_aether_register()`. Stubs land first returning `-ENOSYS` (Step 1), then the
real handlers (Step 2) — the table never shrinks.

## 2. Kernel structures (`kernel/aether/aether.h`)

```c
enum aether_status { AE_FREE=0, AE_PENDING, AE_APPROVED, AE_REJECTED, AE_EXPIRED, AE_DONE };
enum aether_action {
    ACTION_NONE=0, ACTION_WRITE_FILE, ACTION_PRINT, ACTION_SPAWN_PROCESS
};

#define AETHER_QUEUE_LEN   256
#define AETHER_AUDIT_LEN   4096
#define AETHER_PAYLOAD_MAX 512
#define AETHER_ACTION_TTL_TICKS 6000u   /* 60 s @ 100 Hz */

struct aether_action_entry {
    uint64_t action_id;     uint32_t agent_pid;  uint32_t action_type;
    uint32_t status;        uint32_t _pad;        uint64_t submit_tick;
    uint8_t  payload[AETHER_PAYLOAD_MAX];
};
struct aether_audit_entry {
    uint64_t timestamp;     uint32_t agent_pid;  uint32_t action_type;
    uint64_t action_id;     uint32_t result;     uint32_t _pad;
};
```

Both arrays are `static` in their `.c` files (kernel BSS). `g_sovereign_mode`
(`u32`, init 1) and the action-id counter are file-static with accessor fns.

## 3. TCB additions (append at struct end — preserves offsets)

```c
uint64_t mem_limit;        /* hard cap, bytes; 0 = use AETHER_MEM_DEFAULT (128 MiB) */
uint64_t mem_used;         /* charged by mmap/brk growth                            */
uint32_t is_agent;         /* 1 if CAP_AGENT process (rate-limited + agent syscalls)*/
uint32_t sc_count;         /* syscalls in the current 1 s window                    */
uint64_t sc_window_start;  /* g_ticks at window open                                */
```

`sched_create_user*` zero these (kmalloc'd TCB already zeroed); `mem_limit`
defaults lazily (0 → 128 MiB) so existing callers need no change.

## 4. Control flow

**Submit (`SYS_SUBMIT_ACTION`)**: require `CAP_AGENT` (else `-EPERM` + audit
`cap-escalation-denied`). `copyin` ≤512 B payload. Find a `FREE` slot (linear
scan); none → `-EAGAIN`. Fill entry, `submit_tick=g_ticks`. If
`action_type==ACTION_SPAWN_PROCESS` → force `AE_PENDING` (D8). Else if
`g_sovereign_mode` → `AE_APPROVED` + audit `approve`. Else `AE_PENDING` + audit
`submit`. Return `action_id`.

**Poll (`SYS_POLL_RESULT`)**: locate by `action_id`; `agent_pid` must equal caller
(else `-ESRCH`). First expire: if `PENDING` and `g_ticks-submit_tick >
TTL` → `AE_EXPIRED` + audit. Return status. On `APPROVED`/`REJECTED`/`EXPIRED` the
slot is freed *after* the agent has read it once (status latched then `AE_FREE`)
— actually: keep until agent acts; freed on a second poll of a terminal state, or
on agent exit. (Simplicity: free on terminal-state read.)

**Approve/Reject**: require `CAP_SOVEREIGN` (else `-EPERM` + audit). Flip
`PENDING`→`APPROVED|REJECTED`, audit the decision.

**Mem charge**: `aether_mem_charge(t, bytes)` in `sys_mmap` before mapping; if
`t->mem_used+bytes > limit` → audit `oom`, serial `AGENT_OOM_KILLED PID=N`,
`sched_exit(137)`-style kill (the process never returns to ring 3). Uncharge on
`munmap`/teardown.

**Rate limit** (dispatcher, agents only): on entry, if `g_ticks - sc_window_start
>= 100` reset window. `++sc_count`; if `> 60` → serial `AGENT_RATE_LIMITED
PID=N` + audit + clean kill.

## 5. Userspace

`user/aether_daemon.c` (musl, like `init.c`): direct `syscall()` wrappers for the
AETHER NSI. Reads `/etc/aether/config` (SFS) — `key=value` lines: `model`,
`ollama_host`, `test_mode`. Prints `PRADYOS_AETHER_DAEMON_OK`. In test mode
(`test_mode=1`, overridable by kernel cmdline token `aether_test`) it
`SYS_SPAWN_AGENT("/agent_base","test")` once, then enters its IPC/reaper loop
exposing `AETHER_SPAWN/STATUS/KILL/MODE`.

`user/agent_base.c` (musl): `argv[1]=model argv[2]=task`. Test mode → fixed
response string. Parse `ACTION: WRITE_FILE <path> <data>` → `SYS_SUBMIT_ACTION
(ACTION_WRITE_FILE, "<path>\0<data>")`. Poll every 100 ms (`SYS_YIELD` spin) until
terminal. On `APPROVED`: execute (open/write the file via `SYS_OPEN`/`SYS_WRITE`).
Print `PRADYOS_AGENT_DONE`, `exit(0)`. Live mode: lwIP TCP connect to
`ollama_host`, HTTP/1.1 `POST /api/generate`, scan JSON `"response"`.

## 6. Image plumbing
`/etc/aether/config` and the `agent_base`/`aether_daemon` ELFs are placed on the
**SFS** image at `make image` (SFS large-read is reliable; FAT32 large-read is
not — see SESSION_HANDOFF). The daemon is launched by the kernel/init the same
proven way PRISM is (`elf_load` from SFS), as init's child.

## 7. Gates

- **`smoke-aether-queue`**: a kernel in-boot self-test (`aether_selftest`) submits
  an `ACTION_PRINT`, asserts sovereign auto-approve + an audit entry, prints
  `PRADYOS_AETHER_QUEUE_OK`. Pure kernel — no userspace needed, runs early.
- **`smoke-aether`**: boot with `aether_test`; grep serial for
  `PRADYOS_AETHER_QUEUE_OK`, `PRADYOS_AETHER_DAEMON_OK`, `PRADYOS_AGENT_DONE` and
  the written file's marker `PRADYOS_AGENT_VERIFIED`.
- **`smoke-aether-sec`**: kernel security self-test (`aether_sectest`) +
  assertions: OOM (`AGENT_OOM_KILLED`), cap-escalation denied + survives,
  rate-limit (`AGENT_RATE_LIMITED`), queue overflow (257th → `-EAGAIN`), audit
  wrap (`AETHER_AUDIT_WRAP`). No panic string allowed.

Each gate uses `tools/qemu_runner/boot_test.sh` with `EXTRA_SENTINEL` like the
NET-B gates. CI adds the three steps after the NET-B steps.

## 8. Build order (commits)
1. ADR-026 + DDR-AETHER (this).  2. `kernel/aether/` + dispatcher hooks + stubs→impl.
3. `sys_aether.c` handlers wired into `syscall_init`.  4. `aether_daemon.c`.
5. `agent_base.c`.  6. gates + CI + SFS image plumbing.  7. docs + handoff.
