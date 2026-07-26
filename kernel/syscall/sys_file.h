/* kernel/syscall/sys_file.h — sys_open / sys_close / sys_fstat (Phase 5b). */
#pragma once

/* Open flags, Linux values. O_CREAT is honoured in sys_open (DDR-744); O_TRUNC
 * likewise (DDR-782), while O_APPEND is honoured by the FD_VFS write path in
 * sys_io.c — hence these live in the header rather than in one .c file. */
#define O_CREAT  0x40                         /* octal 0100  */
#define O_TRUNC  0x200                        /* octal 01000 */
#define O_APPEND 0x400                        /* octal 02000 */

/* Register SYS_OPEN / SYS_CLOSE / SYS_FSTAT (called by syscall_init). */
void sys_file_register(void);
