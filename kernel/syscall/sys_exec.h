/* kernel/syscall/sys_exec.h — sys_execve registration (Phase 5b slice 7).
 *
 * sys_execve replaces the calling process's image with a new ELF read from the
 * process root filesystem. The handler is file-static in sys_exec.c; only its
 * table registration is public.
 */
#pragma once

void sys_exec_register(void);
