/* kernel/cap.h — NEXUS Capability System (NCS), Phase 2c.
 *
 * Capabilities are opaque 64-bit handles = (generation << 32 | slot index) into
 * a per-process, kernel-private capability table. Userspace can never fabricate
 * authority: a handle is only valid if its slot is in use AND its generation
 * matches the slot's current generation. Revocation is O(1) (bump the slot
 * generation). Delegation/restriction can only ever subset rights, never amplify
 * them (no confused-deputy). The blueprint's 128-bit MAC token is treated as a
 * future external wire format, not the internal representation (ADR-009).
 */
#pragma once
#include <stdint.h>

typedef uint64_t cap_t;
#define CAP_NULL 0ull

/* Rights bitmap (extensible). */
#define CAP_FILE_R        (1u << 0)
#define CAP_FILE_W        (1u << 1)
#define CAP_NET           (1u << 2)
#define CAP_PROCESS_SPAWN (1u << 3)
#define CAP_KERNEL_QUERY  (1u << 4)
#define CAP_DISPLAY       (1u << 5)
#define CAP_HARDWARE_READ (1u << 6)
#define CAP_IPC_SEND      (1u << 7)
#define CAP_IPC_RECV      (1u << 8)
#define CAP_BROADCAST     (1u << 9)   /* publish to the sovereign broadcast bus */
#define CAP_FS_READ       (1u << 10)  /* read files/dirs via the VFS            */
#define CAP_FS_WRITE      (1u << 11)  /* write/create/delete via the VFS        */
#define CAP_FS_ADMIN      (1u << 12)  /* mount/format                           */
/* SOVEREIGN FS (SFS) rights — reserved ahead of the SFS engine (Phase 4).
 * Distinct from the generic CAP_FS_* so SFS-native operations (snapshots,
 * transactions, compression policy) can be gated independently. */
#define CAP_FS_SFS_READ   (1u << 13)  /* read an SFS volume                     */
#define CAP_FS_SFS_WRITE  (1u << 14)  /* write/transact on an SFS volume         */
#define CAP_FS_SFS_ADMIN  (1u << 15)  /* mkfs/snapshot/compaction on SFS         */
/* AETHER agent layer (Layer 6, ADR-026 §D6). Vocabulary bits; an agent's actual
 * possession is the kernel-set per-process flag (tcb.is_agent / is_sovereign) so
 * a process can never mint these into its own table (no self-escalation). */
#define CAP_SOVEREIGN     (1u << 16)  /* change global mode; approve/reject actions */
#define CAP_AGENT         (1u << 17)  /* submit/poll actions; spawn/kill agents      */
/* DDR-836: agent memory (NSI 82/83). NOTE the scope — the store is a SHARED
 * blackboard, so this bit grants read AND overwrite of EVERY key, not of some
 * per-agent slice. Grant it accordingly. */
#define CAP_MEMORY        (1u << 18)  /* read/write the agent memory store           */
/* DDR-842: code rewrite. ALWAYS requires CAP_SOVEREIGN co-approval and is neve
 * auto-granted — the syscall checks both bits, and sovereignty alone is refused
 * so that this bit is not decoration. */
#define CAP_REWRITE       (1u << 21)  /* approve MOSS-style code rewrites            */

/* DDR-982: per-agent action authority. Bits 19/20/22/23 are the free ones —
 * 21 is CAP_REWRITE above, which is why the directive names exactly these four.
 *
 * These gate ACTION SUBMISSION (aether_submit), not the action implementations.
 * That ordering is deliberate: the boundary is enforceable and testable now,
 * and stays correct when the implementations land. Three of the four gate an
 * action whose implementation is out of scope (ACTION_BROWSE_WEB is the
 * deferred cloud bridge, DDR-793; PARSE_DOCUMENT needs a 64 MiB model;
 * QUERY_SCENE needs a scene graph) — a denial path with an audit record is a
 * shipped capability, not a deferred one. See DDR-982 §3. */
#define CAP_OCR           (1u << 19)  /* AHNIS  — ACTION_PARSE_DOCUMENT   */
#define CAP_EXEC          (1u << 20)  /* PRAX   — ACTION_EXEC_CODE        */
#define CAP_SCENE         (1u << 22)  /* IRIS   — ACTION_QUERY_SCENE      */
#define CAP_NET_BROWSE    (1u << 23)  /* LUMYN  — ACTION_BROWSE_WEB       */

/* Resource types a capability can refer to. */
#define RES_NONE   0u
#define RES_FILE   1u
#define RES_IPC    2u
#define RES_DEVICE 3u
/* DDR-1034: the bounded experiment executor. One res_id for the whole
 * subsystem, so the capability grants "may run experiments", not "may run
 * THIS experiment" -- the same coarseness DDR-1033 recorded for RES_IPC,
 * stated here rather than implied. */
#define RES_EXEC   4u

struct cap_table;   /* opaque; kernel-private */

struct cap_table *cap_table_create(void);
void              cap_table_destroy(struct cap_table *t);

/* Mint a new capability for (res_type, res_id) with `rights`. */
cap_t    cap_create(struct cap_table *t, uint32_t res_type, uint64_t res_id, uint32_t rights);
/* New handle for the same resource with rights = old & keep_rights (subset). */
cap_t    cap_restrict(struct cap_table *t, cap_t h, uint32_t keep_rights);
/* Copy into dst with rights = src & subset_rights (cannot amplify). */
cap_t    cap_delegate(struct cap_table *src, cap_t h, struct cap_table *dst, uint32_t subset_rights);
/* fork: make `dst` an exact structural copy of `src` (every slot + generation),
 * so all of the parent's handles stay valid verbatim in the child. Rights are
 * equal (no escalation); the tables are separate (child cannot revoke parent
 * caps). Returns 0 on success, -1 on a NULL table. */
int      cap_fork(const struct cap_table *src, struct cap_table *dst);
/* O(1) revoke: invalidates all outstanding handles for this slot. */
int      cap_revoke(struct cap_table *t, cap_t h);
/* 1 if the handle is valid AND holds all `required` rights, else 0. */
uint32_t cap_validate(struct cap_table *t, cap_t h, uint32_t required_rights);
/* Like cap_validate, but also requires the cap to refer to exactly
 * (res_type, res_id) — binds authority to a specific resource. */
uint32_t cap_authorize(struct cap_table *t, cap_t h, uint32_t res_type,
                       uint64_t res_id, uint32_t required_rights);
