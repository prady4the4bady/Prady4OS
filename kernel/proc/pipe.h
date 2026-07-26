/* kernel/proc/pipe.h — anonymous pipes + dup2 (Phase 5b, PROC-A).
 *
 * A pipe is a 4 KiB byte ring shared by a read-end fd and a write-end fd. The
 * object is reference-counted by the fds that name it (pipe(), dup2(), fork via
 * fd_clone), and freed when the last fd closes.
 *
 * DDR-787: the two ends are counted SEPARATELY. That is what makes blocking
 * bounded (S2): a reader may wait only while a writer still exists, and a writer
 * only while a reader still exists — otherwise it would wait forever. Every
 * incref/close therefore has to say which end it is holding; getting that
 * bookkeeping wrong leaks a pipe or frees it early, so the role is an explicit
 * parameter rather than something inferred inside the pipe code.
 */
#pragma once
#include <stdint.h>

struct pipe;

#define PIPE_WRITE_END 1            /* stored in fd_entry.flags for the write fd */

struct pipe *pipe_create(void);     /* 0 readers, 0 writers; each fd install increfs */
/* is_write != 0 counts the WRITE end, else the read end. Pass
 * (e->flags == PIPE_WRITE_END) from an fd_entry. */
void  pipe_incref(struct pipe *p, int is_write);
void  pipe_close (struct pipe *p, int is_write);  /* frees once BOTH counts hit 0 */
void  pipe_destroy(struct pipe *p);  /* never-installed pipe (pipe() error path) */
long  pipe_read(struct pipe *p, void *dst, uint64_t n);        /* bytes read */
long  pipe_write(struct pipe *p, const void *src, uint64_t n); /* bytes written */
int   pipe_writers(struct pipe *p);  /* DDR-787: >0 while a writer end is open */
int   pipe_readers(struct pipe *p);  /* DDR-787: >0 while a reader end is open */
int   pipe_full(struct pipe *p);     /* DDR-787: ring has no space for a byte */
int   pipe_has_data(struct pipe *p);                          /* 1 if readable (epoll) */

void  pipe_register(void);          /* register SYS_PIPE / SYS_DUP2 */
