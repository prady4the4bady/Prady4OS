#!/usr/bin/env python3
"""Revert CHANGES K and L. Keep A,B,C,D,E,F,G,H,I,J (virtio-blk + primitive).

bcast_wait() returns void and fills an out-parameter; ipc_recv() overloads -1.
Returning early on timeout therefore hands the caller an UNFILLED buffer with no
way to detect it. That is unsafe by inspection, so these two sites go back to
sched_block_on until the API can signal timeout properly.
The bcast `pending` flag and the errno includes stay: they are harmless and the
flag is needed when these sites are bounded correctly later.
"""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "kernel/ipc/bcast.c"; s = rd(p)
s = sub(s,
    "        if (sched_block_timeout(&s->lock, &s->pending, 100) == -ETIMEDOUT) {\n"
    "            s->waiter = 0;\n"
    "            spin_unlock_irqrestore(&s->lock, flags);\n"
    "            return;\n"
    "        }\n",
    "        sched_block_on(&s->lock);        /* publishes BLOCKED under the lock, then sleeps */\n"
    "        /* DDR-955: NOT bounded. bcast_wait returns void and fills *out, so an\n"
    "         * early timeout return would hand the caller an UNFILLED buffer with no\n"
    "         * way to detect it. Bounding this needs a return value first. */\n",
    "L-revert")
wr(p, s); print("L reverted")

p = "kernel/ipc/ipc.c"; s = rd(p)
s = sub(s,
    "        if (sched_block_timeout(&e->lock, &e->full, 100) == -ETIMEDOUT) {\n"
    "            e->waiting_receiver = 0;\n"
    "            spin_unlock_irqrestore(&e->lock, flags);\n"
    "            return -1;\n"
    "        }\n",
    "        sched_block_on(&e->lock);  /* publishes BLOCKED under the lock, then sleeps */\n"
    "        /* DDR-955: NOT bounded. ipc_recv's -1 already means \"cap denied\", so a\n"
    "         * timeout returning -1 is indistinguishable from a permission failure.\n"
    "         * Bounding this needs a distinct return code first. */\n",
    "K-revert")
wr(p, s); print("K reverted")
