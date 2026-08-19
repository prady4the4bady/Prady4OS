import io
p = "kernel/proc/sched.c"
s = io.open(p, encoding="utf-8", newline="").read()
assert '#include "errno.h"' not in s, "already included"
old = '#include "sched.h"\n'
assert s.count(old) == 1
s = s.replace(old, old + '#include "errno.h"     /* DDR-955: ETIMEDOUT for sched_block_timeout */\n', 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("errno.h included in sched.c")
