import io
p = "kernel/drivers/blk/virtio_blk.c"
s = io.open(p, encoding="utf-8", newline="").read()
assert '#include "errno.h"' not in s, "already included"
i = s.index('#include'); j = s.index('\n', i) + 1
s = s[:j] + '#include "errno.h"     /* DDR-955: ETIMEDOUT / EIO */\n' + s[j:]
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("errno.h added to virtio_blk.c")
