/* kernel/syscall/sys_file.h — sys_open / sys_close / sys_fstat (Phase 5b). */
#pragma once

/* Register SYS_OPEN / SYS_CLOSE / SYS_FSTAT (called by syscall_init). */
void sys_file_register(void);
