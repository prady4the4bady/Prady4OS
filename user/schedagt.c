/* user/schedagt.c — the agent half of smoke-schedclass arm H (DDR-1188
 * Amendment 4, K1). The launcher starts this copy with is_agent set and no
 * RES_SCHED. It asks for SCHED_FIFO, which must be refused, reports, then
 * stays alive long enough for SCHEDTEST to try (and fail) to grant it
 * RES_SCHED by pid. Few syscalls: agents are rate-limited (AETHER_RATE_MAX).
 * QEMU (TCG) only; not verified on real hardware.
 */
#include "pradyos.h"

#define nsi pradyos_syscall
#define EPERM 1

struct ts { long sec, nsec; };

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long rc = nsi(SYS_SCHED_SETSCHED, 0, 1, 10);
    static const char ok[]  = "PRADYOS_SCHED_HA eperm=1\n";
    static const char bad[] = "PRADYOS_SCHED_HA eperm=0\n";
    if (rc == -EPERM)
        nsi(SYS_WRITE, 1, (long)ok, sizeof ok - 1);
    else
        nsi(SYS_WRITE, 1, (long)bad, sizeof bad - 1);
    struct ts t = { 60, 0 };
    nsi(SYS_NANOSLEEP, (long)&t, 0, 0);
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
