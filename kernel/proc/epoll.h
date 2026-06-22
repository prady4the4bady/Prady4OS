/* kernel/proc/epoll.h — minimal epoll event loop (Phase 5b, PROC-B).
 *
 * An epoll instance is an fd (FD_EPOLL) carrying a fixed interest table. Baseline
 * scope: edge-agnostic level poll, EPOLLIN on FD_PIPE read-ends (ready iff the
 * ring has bytes); epoll_wait is non-blocking (returns the ready set now). This
 * is the event-loop foundation the agent runtime will build on.
 */
#pragma once
#include <stdint.h>

struct epoll;

void epoll_register(void);   /* SYS_EPOLL_CREATE / _CTL / _WAIT */
