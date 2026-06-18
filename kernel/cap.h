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

/* Resource types a capability can refer to. */
#define RES_NONE   0u
#define RES_FILE   1u
#define RES_IPC    2u
#define RES_DEVICE 3u

struct cap_table;   /* opaque; kernel-private */

struct cap_table *cap_table_create(void);
void              cap_table_destroy(struct cap_table *t);

/* Mint a new capability for (res_type, res_id) with `rights`. */
cap_t    cap_create(struct cap_table *t, uint32_t res_type, uint64_t res_id, uint32_t rights);
/* New handle for the same resource with rights = old & keep_rights (subset). */
cap_t    cap_restrict(struct cap_table *t, cap_t h, uint32_t keep_rights);
/* Copy into dst with rights = src & subset_rights (cannot amplify). */
cap_t    cap_delegate(struct cap_table *src, cap_t h, struct cap_table *dst, uint32_t subset_rights);
/* O(1) revoke: invalidates all outstanding handles for this slot. */
int      cap_revoke(struct cap_table *t, cap_t h);
/* 1 if the handle is valid AND holds all `required` rights, else 0. */
uint32_t cap_validate(struct cap_table *t, cap_t h, uint32_t required_rights);
/* Like cap_validate, but also requires the cap to refer to exactly
 * (res_type, res_id) — binds authority to a specific resource. */
uint32_t cap_authorize(struct cap_table *t, cap_t h, uint32_t res_type,
                       uint64_t res_id, uint32_t required_rights);
