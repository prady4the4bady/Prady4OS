; user/systest.asm — ring-3 syscall test program (Phase 5b, ADR-022).
; ============================================================================
; A single static ELF (linked at 0x8000000000 like hello/wxviol) that exercises
; the 5b syscalls and prints a sentinel for each outcome. The kernel writes it to
; SFS and loads it back, so it runs in its own W^X address space. This program
; GROWS one slice at a time; each slice appends its tests before sys_exit and the
; matching `smoke-sys*` gate greps the new sentinel lines.
;
; Syscall ABI: number in RAX; args RDI, RSI, RDX, R10; return in RAX.
; ============================================================================

BITS 64

SYS_WRITE equ 6
SYS_EXIT  equ 4
SYS_READ   equ 5
SYS_OPEN   equ 7
SYS_CLOSE  equ 8
SYS_FSTAT  equ 9
SYS_GETPID equ 2
SYS_LSEEK  equ 10
SYS_GETCWD equ 11
SYS_MMAP   equ 12
SYS_MUNMAP equ 13
SYS_EXECVE equ 14

MMAP_HINT  equ 0x8800000000      ; VMM_MMAP_BASE (544 GiB)
EINVAL     equ 22               ; returned as -EINVAL

STDOUT    equ 1
ENOENT    equ 2          ; returned as -ENOENT
EBADF     equ 9          ; returned as -EBADF
EFAULT    equ 14         ; returned as -EFAULT

section .text
global _start
_start:
    ; ---- slice 3: sys_write -------------------------------------------------
    ; Good write to stdout — this line is the headline sentinel.
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_write]
    mov     rdx, m_write_len
    syscall

    ; Bad fd (99) -> -EBADF; on match, print the EBADF sentinel.
    mov     rax, SYS_WRITE
    mov     rdi, 99
    lea     rsi, [rel m_write]
    mov     rdx, m_write_len
    syscall
    cmp     rax, -EBADF
    jne     .no_ebadf
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_ebadf]
    mov     rdx, m_ebadf_len
    syscall
.no_ebadf:

    ; Bad buffer (NULL) -> -EFAULT; on match, print the EFAULT sentinel.
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    xor     rsi, rsi
    mov     rdx, 8
    syscall
    cmp     rax, -EFAULT
    jne     .no_efault
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_efault]
    mov     rdx, m_efault_len
    syscall
.no_efault:

    ; ---- slice 4: open / fstat / read / close -------------------------------
    sub     rsp, 256               ; scratch: [rbx]=statbuf(144), [rbx+160]=rbuf
    mov     rbx, rsp               ; rbx survives syscalls (callee-saved)

    mov     rax, SYS_OPEN          ; open("/HELLO.TXT", 0, 0)
    lea     rdi, [rel p_hello]
    xor     rsi, rsi
    xor     rdx, rdx
    syscall
    mov     r12, rax               ; r12 = fd (preserved across syscalls)
    cmp     rax, 3
    jl      .sf_done               ; open failed -> skip the file tests
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_open]
    mov     rdx, m_open_len
    syscall

    mov     rax, SYS_FSTAT         ; fstat(fd, statbuf)
    mov     rdi, r12
    mov     rsi, rbx
    syscall
    test    rax, rax
    jnz     .sf_after_fstat
    mov     rax, [rbx + 48]        ; st_size (offset 48 in struct stat)
    test    rax, rax
    jz      .sf_after_fstat
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_fstat]
    mov     rdx, m_fstat_len
    syscall
.sf_after_fstat:

    mov     rax, SYS_READ          ; read(fd, rbuf, 4)
    mov     rdi, r12
    lea     rsi, [rbx + 160]
    mov     rdx, 4
    syscall
    cmp     rax, 4
    jne     .sf_after_read
    cmp     byte [rbx + 160], 0x50 ; 'P' — HELLO.TXT begins "PRADYOS..."
    jne     .sf_after_read
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_read]
    mov     rdx, m_read_len
    syscall
.sf_after_read:

    mov     rax, SYS_CLOSE         ; close(fd)
    mov     rdi, r12
    syscall
    test    rax, rax
    jnz     .sf_after_close
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_close]
    mov     rdx, m_close_len
    syscall
.sf_after_close:

    mov     rax, SYS_OPEN          ; open of a missing path -> -ENOENT
    lea     rdi, [rel p_nope]
    xor     rsi, rsi
    xor     rdx, rdx
    syscall
    cmp     rax, -ENOENT
    jne     .sf_done
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_noent]
    mov     rdx, m_noent_len
    syscall
.sf_done:
    add     rsp, 256

    ; ---- slice 5: getpid / getcwd / lseek -----------------------------------
    sub     rsp, 64
    mov     rbx, rsp               ; rbx = scratch (survives syscalls)

    mov     rax, SYS_GETPID        ; getpid() -> pid > 0
    syscall
    test    rax, rax
    jle     .s5_after_pid
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_getpid]
    mov     rdx, m_getpid_len
    syscall
.s5_after_pid:

    mov     rax, SYS_GETCWD        ; getcwd(buf, 64) -> 2, buf = "/"
    mov     rdi, rbx
    mov     rsi, 64
    syscall
    cmp     rax, 2
    jne     .s5_after_cwd
    cmp     byte [rbx], 0x2F       ; '/'
    jne     .s5_after_cwd
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_getcwd]
    mov     rdx, m_getcwd_len
    syscall
.s5_after_cwd:

    mov     rax, SYS_OPEN          ; open, lseek to 1, read 1 -> 'R'
    lea     rdi, [rel p_hello]
    xor     rsi, rsi
    xor     rdx, rdx
    syscall
    mov     r12, rax
    cmp     rax, 3
    jl      .s5_done
    mov     rax, SYS_LSEEK
    mov     rdi, r12
    mov     rsi, 1
    xor     rdx, rdx               ; SEEK_SET
    syscall
    cmp     rax, 1
    jne     .s5_close
    mov     rax, SYS_READ
    mov     rdi, r12
    mov     rsi, rbx
    mov     rdx, 1
    syscall
    cmp     rax, 1
    jne     .s5_close
    cmp     byte [rbx], 0x52       ; 'R' (HELLO.TXT[1])
    jne     .s5_close
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_lseek]
    mov     rdx, m_lseek_len
    syscall
.s5_close:
    mov     rax, SYS_CLOSE
    mov     rdi, r12
    syscall
.s5_done:
    add     rsp, 64

    ; ---- slice 6: mmap / munmap (MAP_ANON RW+NX) ----------------------------
    mov     r13, MMAP_HINT         ; r13 = hint addr (survives syscalls)

    mov     rax, SYS_MMAP          ; mmap(hint, 4096, RW, ANON|PRIVATE, -1, 0)
    mov     rdi, r13
    mov     rsi, 4096
    mov     rdx, 3                 ; PROT_READ|PROT_WRITE
    mov     r10, 0x22             ; MAP_PRIVATE|MAP_ANONYMOUS
    mov     r8, -1
    xor     r9, r9
    syscall
    mov     r12, rax               ; r12 = mapped addr
    cmp     rax, r13
    jne     .s6_after_map
    mov     rbx, 0x1234567890ABCDEF
    mov     [r12], rbx             ; write the RW+NX page
    mov     rax, [r12]             ; read it back
    cmp     rax, rbx
    jne     .s6_after_map
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_mmap]
    mov     rdx, m_mmap_len
    syscall
.s6_after_map:

    mov     rax, SYS_MMAP          ; PROT_EXEC must be rejected (W^X)
    xor     rdi, rdi
    mov     rsi, 4096
    mov     rdx, 5                 ; PROT_READ|PROT_EXEC
    mov     r10, 0x22
    mov     r8, -1
    xor     r9, r9
    syscall
    cmp     rax, -EINVAL
    jne     .s6_after_wx
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_mmapwx]
    mov     rdx, m_mmapwx_len
    syscall
.s6_after_wx:

    mov     rax, SYS_MUNMAP        ; munmap, then re-mmap the same hint
    mov     rdi, r13
    mov     rsi, 4096
    syscall
    test    rax, rax
    jnz     .s6_done
    mov     rax, SYS_MMAP
    mov     rdi, r13
    mov     rsi, 4096
    mov     rdx, 3
    mov     r10, 0x22
    mov     r8, -1
    xor     r9, r9
    syscall
    cmp     rax, r13
    jne     .s6_done
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_munmap]
    mov     rdx, m_munmap_len
    syscall
.s6_done:

    ; ---- slice 7: execve — replace this image with /EXECTEST.ELF ------------
    ; The kernel placed /EXECTEST.ELF on the FAT32 root. On success execve never
    ; returns: EXECTEST runs in THIS process and prints its own sentinel. The
    ; line below must therefore NEVER appear (the gate greps for its absence).
    mov     rax, SYS_EXECVE
    lea     rdi, [rel p_exec]
    xor     rsi, rsi               ; argv = NULL (baseline)
    xor     rdx, rdx               ; envp = NULL (baseline)
    syscall
    mov     rax, SYS_WRITE         ; only reached if execve FAILED
    mov     rdi, STDOUT
    lea     rsi, [rel m_execbug]
    mov     rdx, m_execbug_len
    syscall

    ; ---- done ---------------------------------------------------------------
    mov     rax, SYS_EXIT
    xor     rdi, rdi
    syscall
.hang:
    jmp     .hang                  ; sys_exit does not return; guard anyway

section .rodata
m_write:     db "SYSWRITE OK", 10
m_write_len: equ $ - m_write
m_ebadf:     db "SYSIO EBADF OK", 10
m_ebadf_len: equ $ - m_ebadf
m_efault:    db "SYSIO EFAULT OK", 10
m_efault_len: equ $ - m_efault
p_hello:     db "/HELLO.TXT", 0
p_nope:      db "/NOPE.TXT", 0
m_open:      db "SYSOPEN OK", 10
m_open_len:  equ $ - m_open
m_fstat:     db "SYSFSTAT OK", 10
m_fstat_len: equ $ - m_fstat
m_read:      db "SYSREAD OK", 10
m_read_len:  equ $ - m_read
m_close:     db "SYSCLOSE OK", 10
m_close_len: equ $ - m_close
m_noent:     db "SYSOPEN ENOENT OK", 10
m_noent_len: equ $ - m_noent
m_getpid:    db "SYSGETPID OK", 10
m_getpid_len: equ $ - m_getpid
m_getcwd:    db "SYSGETCWD OK", 10
m_getcwd_len: equ $ - m_getcwd
m_lseek:     db "SYSLSEEK OK", 10
m_lseek_len: equ $ - m_lseek
m_mmap:      db "SYSMMAP OK", 10
m_mmap_len:  equ $ - m_mmap
m_mmapwx:    db "SYSMMAP WX REJECTED", 10
m_mmapwx_len: equ $ - m_mmapwx
m_munmap:    db "SYSMUNMAP OK", 10
m_munmap_len: equ $ - m_munmap
p_exec:      db "/EXECTEST.ELF", 0
m_execbug:   db "EXECVE: post-exec (BUG)", 10
m_execbug_len: equ $ - m_execbug
