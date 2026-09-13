/* kernel/aether/experiment.h — DDR-1034: ACTION_RUN_EXPERIMENT.
 *
 * A bounded integer stack machine that the KERNEL runs on an agent's behalf,
 * plus a separate results store the agent cannot write.
 *
 * THE SECURITY ARGUMENT IS THE INSTRUCTION SET, NOT A GUARD. There is no LOAD,
 * no STORE and no addressing mode, so "no memory access outside its own stack"
 * is a property of what can be encoded — it cannot be lost by deleting a check.
 * DIV is absent rather than guarded: a #DE in ring 0 is fatal, and omitting the
 * opcode is both cheaper and stronger than checking its operand.
 *
 * Everything else is a bound with an errno: step cap, operand-stack depth,
 * program length, opcode validity. See DDR-1034 §3.
 */
#pragma once
#include <stdint.h>

/* --- opcodes. Wire format: an agent hand-copies these, so they are pinned. --- */
enum exp_op {
    EXP_HALT = 0,   /* stop; top of stack is the result (empty stack -> 0)     */
    EXP_PUSH,       /* + int64 immediate (little-endian, follows the opcode)   */
    EXP_ADD, EXP_SUB, EXP_MUL,
    EXP_DUP, EXP_DROP, EXP_SWAP,
    /* JNZ + int8 relative displacement, applied to the PC of the NEXT
     * instruction. Pops the condition. This is the ONLY opcode that can make
     * the step count exceed the program length -- which is precisely why it
     * exists: without it EXP_MAX_STEPS would be a bound whose only reachable
     * value is the passing one (DDR-1034 §3). */
    EXP_JNZ,
    EXP_OP_MAX
};

/* Bounds. Each forecloses something named in DDR-1034 §3. */
#define EXP_MAX_STEPS   4096u   /* retired instructions; over -> -ELOOP        */
#define EXP_STACK_N       32u   /* operand slots; over -> -EOVERFLOW           */
#define EXP_MAX_CODE     256u   /* program bytes; over -> -EINVAL              */
#define EXP_RESULTS_N     16u   /* result ring slots; wraps (DDR-1034 §5)      */

/* One recorded run. WRITTEN ONLY BY THE EXECUTOR, in the kernel: there is no
 * syscall that writes this, so an agent cannot record a value it did not
 * compute, overwrite another agent's record, or delete one. That is the DDR-812
 * property reproduced by a different mechanism, WITHOUT touching the lockbox. */
typedef struct __attribute__((packed)) {
    uint64_t seq;        /* monotonic, never reused; 0 = slot never written   */
    uint32_t pid;        /* who submitted                                     */
    int32_t  status;     /* 0 = HALT reached, else the negative errno         */
    int64_t  value;      /* top of stack at HALT; meaningless unless status 0 */
    uint32_t steps;      /* instructions retired                              */
    uint32_t code_len;   /* program length as submitted                       */
} exp_result_t;

/* Run `code` (already copied into the kernel). Returns 0 and sets *out on a
 * clean HALT, else a negative errno. Records the outcome either way. */
int  exp_run(const uint8_t *code, uint32_t len, uint32_t pid, int64_t *out);

/* Copy result `idx` out. 0 = ok, -ENOENT = never written, -EINVAL = bad idx. */
int  exp_result_get(uint32_t idx, exp_result_t *out);
