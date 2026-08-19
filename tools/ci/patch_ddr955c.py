#!/usr/bin/env python3
"""DDR-955 part 3: D,L (bcast.c) and K (ipc.c). Indentation verified from the file."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1 match, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "kernel/ipc/bcast.c"; s = rd(p)
assert "s->pending" not in s, "bcast.c: already applied"
s = sub(s, "    s->lock = (spinlock_t)SPINLOCK_INIT;",
        "    s->pending = 0;\n    s->lock = (spinlock_t)SPINLOCK_INIT;", "D1")
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

p = "kernel/ipc/ipc.c"; s = rd(p)
assert "sched_block_timeout" not in s, "ipc.c: already applied"
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
print("ALL 12 CHANGES APPLIED")
