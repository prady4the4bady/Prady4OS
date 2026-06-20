/* kernel/syscall/sys_io.h — sys_read / sys_write registration (Phase 5b). */
#pragma once

/* Register SYS_READ / SYS_WRITE into the dispatch table (called by syscall_init). */
void sys_io_register(void);
