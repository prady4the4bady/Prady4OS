/* kernel/syscall/sys_proc.h — sys_lseek / sys_getcwd (Phase 5b slice 5). */
#pragma once

/* Register SYS_LSEEK / SYS_GETCWD (called by syscall_init). sys_getpid already
 * lives in syscall.c (SYS_GETPID, Phase 2e). */
void sys_proc_register(void);
