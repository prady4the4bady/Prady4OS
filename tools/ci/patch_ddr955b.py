#!/usr/bin/env python3
"""DDR-955 part 2: E,F,G,H,C,D,K,L. A/B/I/J already applied. Asserts on all context."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

# --- E: sched.h ---
p = "kernel/proc/sched.h"; s = rd(p)
assert "block_deadline" not in s, "E: already applied"
i = s.index("struct tcb {"); j = s.index("\n};", i)
s = s[:j] + ("\n    /* DDR-955: bounded-wait fields */\n"
             "    uint64_t   block_deadline;   /* g_ticks + timeout; 0 = no deadline */\n"
             "    int        wake_timed_out;   /* 1 if timer expired before wakeup   */") + s[j:]
s = sub(s, "void        sched_block_on(spinlock_t *lk);",
        "void        sched_block_on(spinlock_t *lk);\n"
        "int         sched_block_timeout(spinlock_t *lk, volatile int *done,\n"
        "                                uint64_t timeout_ticks);", "E2")
wr(p, s); print("E ok")

# --- F,G,H: sched.c ---
p = "kernel/proc/sched.c"; s = rd(p)
assert "sched_block_timeout" not in s, "F/G/H: already applied"
s = sub(s, "    t->blk_wait_next = 0;",
        "    t->block_deadline  = 0;   /* DDR-955 */\n"
        "    t->wake_timed_out  = 0;   /* DDR-955 */\n"
        "    t->blk_wait_next = 0;", "F")
s = sub(s, "    schedule();\n    spin_lock(lk);\n}",
        "    schedule();\n    spin_lock(lk);\n}\n\n"
        "/* DDR-955: sched_block_on with a deadline.\n"
        " * IDENTICAL locking contract: called WITH lk held, returns WITH lk held.\n"
        " * Returns 0 on normal wake, -ETIMEDOUT if the timer expired first. */\n"
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

# --- C: bcast.h ---
p = "kernel/ipc/bcast.h"; s = rd(p)
i = s.index("struct bcast_subscriber {"); j = s.index("\n};", i)
s = s[:j] + "\n    volatile int pending;   /* DDR-955: 1 when queue has an unread item */" + s[j:]
wr(p, s); print("C ok")

# --- D,L: bcast.c ---
p = "kernel/ipc/bcast.c"; s = rd(p)
s = sub(s, "    s->waiter = 0;", "    s->waiter = 0;\n    s->pending = 0;", "D1")
s = sub(s, "    s->tail = next;", "    s->tail = next;\n    s->pending = 1;", "D2")
s = sub(s, "    s->head = (s->head + 1) % BCAST_QUEUE;",
        "    s->head = (s->head + 1) % BCAST_QUEUE;\n"
        "    if (s->head == s->tail) s->pending = 0;", "D3")
s = sub(s, "    while (s->head == s->tail) {\n"
           "        s->waiter = current_thread;\n"
           "        sched_block_on(&s->lock);        /* publishes BLOCKED under the lock, then sleeps */\n"
           "    }",
        "    while (s->head == s->tail) {\n"
        "        s->waiter = current_thread;\n"
        "        if (sched_block_timeout(&s->lock, &s->pending, 100) == -ETIMEDOUT) {\n"
        "            s->waiter = 0;\n"
        "            spin_unlock_irqrestore(&s->lock, flags);\n"
        "            return;\n"
        "        }\n"
        "    }", "L")
wr(p, s); print("D,L ok")

# --- K: ipc.c ---
p = "kernel/ipc/ipc.c"; s = rd(p)
s = sub(s, "    while (!e->full) {\n"
           "        e->waiting_receiver = current_thread;\n"
           "        sched_block_on(&e->lock);  /* publishes BLOCKED under the lock, then sleeps */\n"
           "    }",
        "    while (!e->full) {\n"
        "        e->waiting_receiver = current_thread;\n"
        "        if (sched_block_timeout(&e->lock, &e->full, 100) == -ETIMEDOUT) {\n"
        "            e->waiting_receiver = 0;\n"
        "            spin_unlock_irqrestore(&e->lock, flags);\n"
        "            return -1;\n"
        "        }\n"
        "    }", "K")
wr(p, s); print("K ok")
print("ALL REMAINING CHANGES APPLIED")
