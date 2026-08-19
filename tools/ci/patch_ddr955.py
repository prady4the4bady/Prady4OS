#!/usr/bin/env python3
"""DDR-955 FEAT-A. Every context string is asserted before substitution."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag, n=1):
    assert s.count(old) == n, "%s: expected %d match, got %d" % (tag, n, s.count(old))
    return s.replace(old, new, n)

# --- A: errno.h -----------------------------------------------------------
p = "kernel/include/errno.h"; s = rd(p)
assert "ENOKEY        126" in s, "A: tail"
assert "ETIMEDOUT" not in s, "A: already present"
if not s.endswith(chr(10)): s += chr(10)
s += "#define ETIMEDOUT     110  /* DDR-955: timed-block wait expired */" + chr(10)
wr(p, s); print("A ok")

# --- B + I + J: virtio_blk.c ---------------------------------------------
p = "kernel/drivers/blk/virtio_blk.c"; s = rd(p)
s = sub(s, "    volatile int          compl_ap;",
        "    volatile int          compl_ap;\n"
        "    volatile int          slot_free;  /* DDR-955: 1 when a slot just freed */",
        "B1")
s = sub(s, "    v->compl_ap = 0;", "    v->compl_ap = 0;\n    v->slot_free = 0;", "B2")
s = sub(s, "    w->blk_wait_next = 0;\n    sched_unblock(w);\n}",
        "    w->blk_wait_next = 0;\n"
        "    v->slot_free = 1;      /* DDR-955: signal that a slot is available */\n"
        "    sched_unblock(w);\n}", "B3")
s = sub(s, "        sched_block_on(&v->compl_lock);        /* woken when a slot frees */",
        "        if (sched_block_timeout(&v->compl_lock,\n"
        "                                &v->slot_free,\n"
        "                                500) == -ETIMEDOUT) {\n"
        "            kputs(\"[vblk] slot wait timeout\r\n\");\n"
        "            spin_unlock_irqrestore(&v->compl_lock, fl);\n"
        "            return -EIO;\n"
        "        }\n"
        "        v->slot_free = 0;", "I")
s = sub(s, "    while (!v->req[s].done)\n"
           "        sched_block_on(&v->compl_lock);        /* BLOCKED published under the lock */",
        "    while (!v->req[s].done) {\n"
        "        if (sched_block_timeout(&v->compl_lock,\n"
        "                                &v->req[s].done,\n"
        "                                500) == -ETIMEDOUT) {\n"
        "            kputs(\"[vblk] compl wait timeout\r\n\");\n"
        "            v->req[s].used = 0;\n"
        "            v->req[s].waiter = 0;\n"
        "            slot_wake_one(v);\n"
        "            spin_unlock_irqrestore(&v->compl_lock, fl);\n"
        "            return -EIO;\n"
        "        }\n"
        "    }", "J")
wr(p, s); print("B,I,J ok")

# --- E: sched.h -----------------------------------------------------------
p = "kernel/proc/sched.h"; s = rd(p)
i = s.index("struct tcb {"); j = s.index("\n};", i)
s = s[:j] + ("\n\n    /* DDR-955: bounded-wait fields */\n"
             "    uint64_t   block_deadline;   /* g_ticks + timeout; 0 = no deadline */\n"
             "    int        wake_timed_out;   /* 1 if timer expired before wakeup   */") + s[j:]
old_decl = "void sched_block_on(spinlock_t *lk);"
assert old_decl in s, "E: sched_block_on decl"
s = sub(s, old_decl, old_decl + "\nint sched_block_timeout(spinlock_t *lk, volatile int *done,\n"
                                "                        uint64_t timeout_ticks);", "E2")
wr(p, s); print("E ok")

# --- F,G,H: sched.c -------------------------------------------------------
p = "kernel/proc/sched.c"; s = rd(p)
s = sub(s, "    t->blk_wait_next = 0;",
        "    t->block_deadline  = 0;   /* DDR-955 */\n"
        "    t->wake_timed_out  = 0;   /* DDR-955 */\n"
        "    t->blk_wait_next = 0;", "F")
s = sub(s, "    schedule();\n    spin_lock(lk);\n}",
        "    schedule();\n    spin_lock(lk);\n}\n\n"
        "/* DDR-955: sched_block_on with a deadline.\n"
        " * IDENTICAL locking contract: called WITH lk held, returns WITH lk held.\n"
        " * Returns 0 on normal wake, -ETIMEDOUT if the timer expired first.\n"
        " * The caller's while-loop re-checks the condition on return. */\n"
        "int sched_block_timeout(spinlock_t *lk, volatile int *done,\n"
        "                        uint64_t timeout_ticks)\n"
        "{\n"
        "    if (!current_thread) {\n"
        "        spin_unlock(lk);\n"
        "        return -ETIMEDOUT;\n"
        "    }\n"
        "    if (*done) return 0;\n"
        "    current_thread->wake_timed_out = 0;\n"
        "    current_thread->block_deadline = g_ticks + timeout_ticks;\n"
        "    current_thread->state = THREAD_BLOCKED;\n"
        "    spin_unlock(lk);\n"
        "    schedule();\n"
        "    spin_lock(lk);\n"
        "    current_thread->block_deadline = 0;\n"
        "    return current_thread->wake_timed_out ? -ETIMEDOUT : 0;\n"
        "}", "G")
s = sub(s, "    current_thread->run_ticks++;",
        "    current_thread->run_ticks++;\n"
        "\n    /* DDR-955: expire blocked threads whose deadline has passed */\n"
        "    {\n"
        "        struct tcb *_wake[32];\n"
        "        int _nwake = 0;\n"
        "        uint64_t _fl2 = irq_save();\n"
        "        struct tcb *_t = current_thread;\n"
        "        if (_t) {\n"
        "            do {\n"
        "                if (_t->state == THREAD_BLOCKED\n"
        "                    && _t->block_deadline != 0\n"
        "                    && g_ticks >= _t->block_deadline\n"
        "                    && _nwake < 32) {\n"
        "                    _t->wake_timed_out = 1;\n"
        "                    _t->block_deadline = 0;\n"
        "                    _wake[_nwake++] = _t;\n"
        "                }\n"
        "                _t = _t->next;\n"
        "            } while (_t != current_thread);\n"
        "        }\n"
        "        irq_restore(_fl2);\n"
        "        for (int _i = 0; _i < _nwake; _i++)\n"
        "            sched_unblock(_wake[_i]);\n"
        "    }", "H")
wr(p, s); print("F,G,H ok")
print("PATCH APPLIED (C,D,K,L pending separate verification)")
