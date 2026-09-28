/* kernel/aether/experiment.c — DDR-1034. See experiment.h for the argument. */
#include "experiment.h"
#include <errno.h>

/* The result ring. Kernel-owned; the executor below is its ONLY writer. */
static exp_result_t g_exp[EXP_RESULTS_N];
static uint64_t     g_exp_seq;          /* monotonic; first record gets seq 1 */

/* Record an outcome. static, so the linker makes it unnameable outside this
 * translation unit -- the same structural argument metric_page.c uses for the
 * lockbox writer (DDR-812), applied here so a future caller cannot appear. */
static void exp_record(uint32_t pid, int32_t status, int64_t value,
                       uint32_t steps, uint32_t code_len)
{
    uint64_t seq = ++g_exp_seq;
    exp_result_t *r = &g_exp[(seq - 1) % EXP_RESULTS_N];
    r->pid      = pid;
    r->status   = status;
    r->value    = value;
    r->steps    = steps;
    r->code_len = code_len;
    r->seq      = seq;                  /* published LAST: a reader that sees a
                                         * non-zero seq sees a complete record */
}

/* Little-endian load of an unaligned int64 immediate. The code buffer is a byte
 * array, so the immediate is never guaranteed aligned. */
static int64_t imm64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return (int64_t)v;
}

int exp_run(const uint8_t *code, uint32_t len, uint32_t pid, int64_t *out)
{
    int64_t  st[EXP_STACK_N];
    uint32_t sp    = 0;                 /* next free slot                     */
    uint32_t pc    = 0;
    uint32_t steps = 0;
    int32_t  rc    = 0;
    int64_t  value = 0;

    if (len == 0 || len > EXP_MAX_CODE)
        return -EINVAL;

    for (;;) {
        if (steps >= EXP_MAX_STEPS) {   /* the bound EXP_JNZ makes reachable  */
            rc = -ELOOP;
            break;
        }
        if (pc >= len) {                /* ran off the end without HALT       */
            rc = -EINVAL;
            break;
        }
        uint8_t op = code[pc];
        if (op >= EXP_OP_MAX) {
            rc = -EINVAL;
            break;
        }
        steps++;

        /* Arity check, once, for every opcode that pops. An underflow is a
         * malformed program, not a runtime condition. */
        static const uint8_t need[EXP_OP_MAX] = {
            [EXP_HALT] = 0, [EXP_PUSH] = 0,
            [EXP_ADD]  = 2, [EXP_SUB]  = 2, [EXP_MUL] = 2,
            [EXP_DUP]  = 1, [EXP_DROP] = 1, [EXP_SWAP] = 2,
            [EXP_JNZ]  = 1,
        };
        if (sp < need[op]) {
            rc = -EINVAL;
            break;
        }

        switch (op) {
        case EXP_HALT:
            value = (sp > 0) ? st[sp - 1] : 0;
            rc = 0;
            goto done;
        case EXP_PUSH:
            if (pc + 9 > len)           { rc = -EINVAL;    goto done; }
            if (sp >= EXP_STACK_N)      { rc = -EOVERFLOW; goto done; }
            st[sp++] = imm64(&code[pc + 1]);
            pc += 9;
            continue;
        case EXP_ADD:
            st[sp - 2] = (int64_t)((uint64_t)st[sp - 2] + (uint64_t)st[sp - 1]);
            sp--;
            break;
        case EXP_SUB:
            st[sp - 2] = (int64_t)((uint64_t)st[sp - 2] - (uint64_t)st[sp - 1]);
            sp--;
            break;
        case EXP_MUL:
            st[sp - 2] = (int64_t)((uint64_t)st[sp - 2] * (uint64_t)st[sp - 1]);
            sp--;
            break;
        case EXP_DUP:
            if (sp >= EXP_STACK_N)      { rc = -EOVERFLOW; goto done; }
            st[sp] = st[sp - 1];
            sp++;
            break;
        case EXP_DROP:
            sp--;
            break;
        case EXP_SWAP: {
            int64_t t = st[sp - 1];
            st[sp - 1] = st[sp - 2];
            st[sp - 2] = t;
            break;
        }
        case EXP_JNZ: {
            if (pc + 2 > len)           { rc = -EINVAL; goto done; }
            int64_t cond = st[--sp];
            int32_t disp = (int32_t)(int8_t)code[pc + 1];
            uint32_t next = pc + 2;
            if (cond != 0) {
                int64_t tgt = (int64_t)next + disp;
                if (tgt < 0 || tgt >= (int64_t)len) { rc = -EINVAL; goto done; }
                pc = (uint32_t)tgt;
            } else {
                pc = next;
            }
            continue;
        }
        default:
            rc = -EINVAL;
            goto done;
        }
        pc++;
    }

done:
    exp_record(pid, rc, value, steps, len);
    if (rc == 0 && out)
        *out = value;
    return rc;
}

int exp_result_get(uint32_t idx, exp_result_t *out)
{
    if (idx >= EXP_RESULTS_N || !out)
        return -EINVAL;
    if (g_exp[idx].seq == 0)
        return -ENOENT;
    *out = g_exp[idx];
    return 0;
}
