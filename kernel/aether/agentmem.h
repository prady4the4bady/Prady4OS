/* kernel/aether/agentmem.h — agent memory store (DDR-836, NSI 82/83).
 *
 * A bounded key/value store agents use to persist facts across actions.
 *
 * THIS IS A SHARED BLACKBOARD, NOT PER-AGENT STORAGE. Records are global to all
 * CAP_MEMORY holders: any agent with the capability can read and overwrite any
 * key. The capability is the entire boundary. Keying records by owner pid was
 * considered and rejected — a pid is recycled on exit, so a new agent would
 * silently inherit a dead one's memories, which is worse than no isolation at
 * all because it looks like isolation. Durable per-agent identity arrives with
 * the agent roster (Section G); see DDR-836.
 */
#pragma once
#include <stdint.h>

#define MEM_MAX_RECORDS 64u
#define MEM_MAX_KEY     32u
#define MEM_MAX_VAL     256u

enum {
    MEM_OK        =  0,
    MEM_ERR_ARG   = -1,   /* null / oversized key or value            */
    MEM_ERR_NOENT = -2,   /* no record under that key                 */
    MEM_ERR_NOSPC = -3    /* table full — never evicts, see DDR-836   */
};

/* Store `val` under `key`, REPLACING any existing record with that key. The
 * store is a map, not a log: two records under one key would make "what is X?"
 * depend on scan order. */
int agentmem_write(const char *key, const void *val, uint32_t vallen);

/* Fetch the value under `key`. A missing key is MEM_ERR_NOENT, never a
 * zero-length success — an agent handed an empty "fact" would reason from it. */
int agentmem_read(const char *key, void *out, uint32_t *outlen);
