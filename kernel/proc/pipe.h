/* kernel/proc/pipe.h — anonymous pipes + dup2 (Phase 5b, PROC-A).
 *
 * A pipe is a 4 KiB byte ring shared by a read-end fd and a write-end fd. The
 * object is reference-counted by the fds that name it (pipe(), dup2(), fork via
 * fd_clone), and freed when the last fd closes. Baseline: non-blocking — a read
 * of an empty pipe returns 0, a write to a full pipe returns a short count.
 */
#pragma once
#include <stdint.h>

struct pipe;

#define PIPE_WRITE_END 1            /* stored in fd_entry.flags for the write fd */

struct pipe *pipe_create(void);     /* refcount 0; each fd install increfs */
void  pipe_incref(struct pipe *p);
void  pipe_close(struct pipe *p);   /* decref; frees ring + struct at 0 */
long  pipe_read(struct pipe *p, void *dst, uint64_t n);        /* bytes read */
long  pipe_write(struct pipe *p, const void *src, uint64_t n); /* bytes written */

void  pipe_register(void);          /* register SYS_PIPE / SYS_DUP2 */
