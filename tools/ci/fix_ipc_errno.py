import io
for p, anchor in (("kernel/ipc/ipc.c", None), ("kernel/ipc/bcast.c", None)):
    s = io.open(p, encoding="utf-8", newline="").read()
    if '#include "errno.h"' in s:
        print(p, "already includes errno.h"); continue
    i = s.index('#include')
    j = s.index('\n', i) + 1
    s = s[:j] + '#include "errno.h"     /* DDR-955: ETIMEDOUT */\n' + s[j:]
    io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
    print(p, "errno.h added")
