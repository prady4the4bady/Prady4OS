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
SYS_FORK   equ 15
SYS_WAIT4  equ 16
SYS_PIPE   equ 17
SYS_DUP2   equ 18
SYS_EPOLL_CREATE equ 19
SYS_EPOLL_CTL    equ 20
SYS_EPOLL_WAIT   equ 21
SYS_SIGACTION    equ 22
SYS_KILL         equ 23
SYS_SIGRETURN    equ 24
SYS_IO_URING_SETUP equ 25
SYS_IO_URING_ENTER equ 26
SIGUSR1          equ 10

MMAP_HINT  equ 0x8800000000      ; VMM_MMAP_BASE (544 GiB)
VDSO_VA    equ 0x00007FFFFFF00000 ; read-only vDSO clock page (IMP-C)
EINVAL     equ 22               ; returned as -EINVAL
ENOSYS     equ 38               ; returned as -ENOSYS (DDR-877)

STDOUT    equ 1
ENOENT    equ 2          ; returned as -ENOENT
EBADF     equ 9          ; returned as -EBADF
EFAULT    equ 14         ; returned as -EFAULT
ENODEV    equ 19         ; returned as -ENODEV (DDR-1112: fd is not FD_VFS)

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

    ; ---- DDR-877 (item 19): the 6-arg ABI, proved by REJECTION -------------
    ; The accept arm above already passes r8=-1 and r9=0, so a broken marshal
    ; turns a5 into garbage and mmap fails. That proves the registers arrive,
    ; but not that the kernel READS them — a kernel still discarding fd and
    ; offset passes it unchanged. These arms only pass if a5 and a6 are read
    ; and acted on, and THEY MUST RETURN DIFFERENT ERRNOS so that swapping r8
    ; and r9 in the marshal fails BOTH.
    ;
    ; RESTRUCTURED BY DDR-1112, AND THE REASON MATTERS. File-backed mmap is now
    ; implemented, so the old arm's `fd=3` no longer answers -ENOSYS and the
    ; two arms would have COLLAPSED ONTO ONE ERRNO (-EINVAL), destroying exactly
    ; the swap-detection DDR-877 designed them for. The fd arm therefore moves
    ; to an fd that cannot be open (99 > FD_MAX 64) and asks for a genuine
    ; file-backed map (MAP_PRIVATE, no MAP_ANONYMOUS), which answers -EBADF.
    ; Swap check, re-derived rather than inherited: with r8/r9 exchanged the fd
    ; arm sees fd=0 (the console) and gets -ENODEV, and the off arm sees fd=4096
    ; and gets -EBADF — so both still fail. Three distinct errnos are now in
    ; play (-EBADF / -ENODEV / -EINVAL), each naming its own family (DDR-1080).
    mov     rax, SYS_MMAP          ; fd=99 (> FD_MAX) -> not an open descriptor
    xor     rdi, rdi
    mov     rsi, 4096
    mov     rdx, 1                 ; PROT_READ
    mov     r10, 0x02              ; MAP_PRIVATE, file-backed (no ANONYMOUS)
    mov     r8, 99
    xor     r9, r9
    syscall
    cmp     rax, -EBADF
    jne     .s6_after_fd
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_mmapfd]
    mov     rdx, m_mmapfd_len
    syscall
.s6_after_fd:

    ; DDR-1112: a pipe or the console has no byte at an offset, so a file-backed
    ; map of one is -ENODEV rather than -EBADF. This arm is NOT a marshal check
    ; (a swap leaves it on the console either way) — it exists to pin the
    ; FD_VFS restriction, which nothing else asserts.
    mov     rax, SYS_MMAP          ; fd=1 is STDOUT -> FD_CONSOLE, not FD_VFS
    xor     rdi, rdi
    mov     rsi, 4096
    mov     rdx, 1                 ; PROT_READ
    mov     r10, 0x02              ; MAP_PRIVATE, file-backed
    mov     r8, 1
    xor     r9, r9
    syscall
    cmp     rax, -ENODEV
    jne     .s6_after_nd
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_mmapnd]
    mov     rdx, m_mmapnd_len
    syscall
.s6_after_nd:

    mov     rax, SYS_MMAP          ; offset != 0 is meaningless for anon
    xor     rdi, rdi
    mov     rsi, 4096
    mov     rdx, 3
    mov     r10, 0x22
    mov     r8, -1
    mov     r9, 4096
    syscall
    cmp     rax, -EINVAL
    jne     .s6_after_off
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_mmapoff]
    mov     rdx, m_mmapoff_len
    syscall
.s6_after_off:

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

    ; ---- DDR-1112: file-backed MAP_PRIVATE, the arms that carry the claim ---
    ; "mmap a file and assert it succeeds" is WEAK in the exact way DDR-877
    ; named: a kernel that accepts the fd and hands back ANONYMOUS ZERO PAGES
    ; passes it. So these assert THE FILE'S OWN BYTES, which such a kernel
    ; cannot produce.
    ;
    ; The cursor arm is the non-obvious one and it is deliberately taken AFTER a
    ; read, so the saved position is 4 and not 0 — a kernel that RESET the
    ; cursor would pass a 0-before/0-after comparison. vfs_read is pread-style
    ; (explicit offset, no cursor in struct vfs_file), so a correct mapping
    ; cannot move it; a build that "simplified" the fill to use e->off would.
    mov     rax, SYS_OPEN          ; open("/HELLO.TXT", 0, 0)
    lea     rdi, [rel p_hello]
    xor     rsi, rsi
    xor     rdx, rdx
    syscall
    cmp     rax, 3
    jl      .s6f_done
    mov     r12, rax               ; r12 = fd

    sub     rsp, 16
    mov     rax, SYS_READ          ; advance the cursor to 4, so "unchanged"
    mov     rdi, r12               ; is a real claim rather than 0 == 0
    mov     rsi, rsp
    mov     rdx, 4
    syscall
    add     rsp, 16
    cmp     rax, 4
    jne     .s6f_close

    ; HINT, NOT NULL, AND THE REASON IS MEASURED. The slice-6 arms above map at
    ; the explicit MMAP_HINT and never unmap it, while sys_mmap advances
    ; t->mmap_next ONLY for an addr==0 request — so a kernel-chosen address
    ; still resolves to MMAP_HINT, collides with that live region and is
    ; correctly refused with -EINVAL (no silent replace). The kernel is right;
    ; the probe just has to ask somewhere free. Recorded because it cost a
    ; debugging pass: mmap(NULL) is not guaranteed to find space for a process
    ; that has used explicit hints.
    mov     rax, SYS_MMAP          ; mmap(hint, 4096, PROT_READ, MAP_PRIVATE, fd, 0)
    mov     rdi, MMAP_HINT
    add     rdi, 0x100000          ; 1 MiB clear of the slice-6 region
    mov     rsi, 4096
    mov     rdx, 1                 ; PROT_READ
    mov     r10, 0x02              ; MAP_PRIVATE, file-backed
    mov     r8, r12
    xor     r9, r9
    syscall
    cmp     rax, 0
    jle     .s6f_close
    mov     r13, rax               ; r13 = mapped address

    lea     rsi, [rel m_hellobytes]   ; ARM A(i): the file's own 25 bytes
    mov     rdi, r13
    mov     rcx, m_hellobytes_len
    repe    cmpsb
    jne     .s6f_unmap

    cmp     byte [r13 + 4095], 0      ; ARM A(ii): the POSIX EOF tail is zero
    jne     .s6f_unmap

    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_mmapfile]
    mov     rdx, m_mmapfile_len
    syscall

    mov     rax, SYS_LSEEK         ; ARM C: the fd cursor is UNCHANGED (still 4)
    mov     rdi, r12
    xor     rsi, rsi
    mov     rdx, 1                 ; SEEK_CUR
    syscall
    cmp     rax, 4
    jne     .s6f_unmap
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_mmapcur]
    mov     rdx, m_mmapcur_len
    syscall

.s6f_unmap:
    mov     rax, SYS_MUNMAP
    mov     rdi, r13
    mov     rsi, 4096
    syscall
.s6f_close:
    mov     rax, SYS_CLOSE
    mov     rdi, r12
    syscall
.s6f_done:

    ; ARM B: a NON-ZERO, page-aligned offset, on a MULTI-PAGE file. /BIGPAT.BIN
    ; is 64 KiB of (7n + 3 + 31*(n>>8)) & 0xFF — a pattern DDR-973 chose
    ; precisely because plain 7n+3 has period 256, which made every cluster
    ; identical and let a chain-repeat mutant pass. So byte 0 of a map at
    ; offset 4096 must be pattern(4096) = 0xF3, and a kernel that ignored a_off
    ; and read from 0 would show pattern(0) = 0x03 instead.
    mov     rax, SYS_OPEN
    lea     rdi, [rel p_bigpat]
    xor     rsi, rsi
    xor     rdx, rdx
    syscall
    cmp     rax, 3
    jl      .s6b_done
    mov     r12, rax

    mov     rax, SYS_MMAP
    mov     rdi, MMAP_HINT
    add     rdi, 0x200000          ; its own hint; must not collide with arm A
    mov     rsi, 4096
    mov     rdx, 1                 ; PROT_READ
    mov     r10, 0x02              ; MAP_PRIVATE, file-backed
    mov     r8, r12
    mov     r9, 4096               ; offset 4096 — the whole point of this arm
    syscall
    cmp     rax, 0
    jle     .s6b_close
    mov     r13, rax

    cmp     byte [r13], 0xF3       ; pattern(4096), NOT pattern(0) = 0x03
    jne     .s6b_unmap
    cmp     byte [r13 + 1], 0xFA   ; pattern(4097) — one more, so a single
    jne     .s6b_unmap             ; lucky byte cannot carry the arm
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_mmapoffrd]
    mov     rdx, m_mmapoffrd_len
    syscall

.s6b_unmap:
    mov     rax, SYS_MUNMAP
    mov     rdi, r13
    mov     rsi, 4096
    syscall
.s6b_close:
    mov     rax, SYS_CLOSE
    mov     rdi, r12
    syscall
.s6b_done:

    ; ---- IMP-C: vDSO clock read (ring-3, no syscall) -----------------------
    ; The kernel maps a read-only clock page at VDSO_VA and the PIT advances
    ; wall_time_ns. A single aligned load is atomic. Print only when non-zero
    ; (a zero read => vDSO not wired => the gate's required line is absent).
    mov     r12, VDSO_VA
    mov     rax, [r12]                 ; wall_time_ns (offset 0)
    test    rax, rax
    jz      .vdso_done
    sub     rsp, 96
    mov     rbx, rsp                   ; rbx = output buffer [0..63]
    lea     rsi, [rel m_vdsop]         ; copy the prefix
    xor     rcx, rcx
.vdso_pre:
    mov     dl, [rsi + rcx]
    mov     [rbx + rcx], dl
    inc     rcx
    cmp     rcx, m_vdsop_len
    jne     .vdso_pre
    lea     r8, [rsp + 88]             ; scratch end; digits grow downward
    mov     r9, r8
    mov     r10, 10
.vdso_div:
    xor     rdx, rdx
    div     r10                        ; rax/=10, rdx=digit
    add     dl, '0'
    dec     r8
    mov     [r8], dl
    test    rax, rax
    jnz     .vdso_div
.vdso_cp:
    mov     dl, [r8]                   ; append digits [r8,r9) after the prefix
    mov     [rbx + rcx], dl
    inc     r8
    inc     rcx
    cmp     r8, r9
    jne     .vdso_cp
    mov     byte [rbx + rcx], 10       ; newline
    inc     rcx
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    mov     rsi, rbx
    mov     rdx, rcx
    syscall
    add     rsp, 96
.vdso_done:

    ; ---- slice 8: fork — child prints + exits, parent prints + continues ----
    ; Placed BEFORE execve: a successful execve replaces this image, so fork must
    ; run first. The child resumes right after this syscall with RAX=0.
    mov     rax, SYS_FORK
    syscall
    test    rax, rax               ; SYSRET restores user RFLAGS — set flags from RAX
    js      .fork_err              ; RAX < 0 -> error
    jz      .fork_child            ; RAX == 0 -> child
    mov     rax, SYS_WRITE         ; parent (RAX = child pid)
    mov     rdi, STDOUT
    lea     rsi, [rel m_forkp]
    mov     rdx, m_forkp_len
    syscall
    jmp     .fork_done
.fork_child:
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_forkc]
    mov     rdx, m_forkc_len
    syscall
    mov     rax, SYS_EXIT
    xor     rdi, rdi
    syscall
.fork_err:
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_forkerr]
    mov     rdx, m_forkerr_len
    syscall
.fork_done:

    ; ---- slice 9: fork + wait4 — parent reaps the child, reads its status ---
    mov     rax, SYS_FORK
    syscall
    test    rax, rax
    js      .w_done                ; fork error -> skip
    jz      .w_child               ; child path
    mov     r12, rax               ; parent: r12 = child pid (callee-saved)
    sub     rsp, 16                ; [rsp] = status slot
    mov     dword [rsp], 0
    mov     rax, SYS_WAIT4
    mov     rdi, r12               ; pid
    mov     rsi, rsp               ; &status
    xor     rdx, rdx               ; options = 0
    syscall
    cmp     dword [rsp], 42        ; child exited 42?
    jne     .w_skip
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_wait]
    mov     rdx, m_wait_len
    syscall
.w_skip:
    add     rsp, 16
    jmp     .w_done
.w_child:
    mov     rax, SYS_EXIT
    mov     rdi, 42
    syscall
.w_done:

    ; ---- PROC-A: pipe + dup2 -----------------------------------------------
    ; pipe() -> [read fd, write fd]; write "PIPE" to the write end, read it back
    ; from the read end. Then dup2 the read end onto fd 30 and round-trip again.
    sub     rsp, 32
    mov     rbx, rsp                ; [rbx]=fds[2], [rbx+16]=rdbuf
    mov     rax, SYS_PIPE
    mov     rdi, rbx
    syscall
    test    rax, rax
    jnz     .pipe_done
    mov     r12d, [rbx]             ; r12 = read fd
    mov     r13d, [rbx+4]           ; r13 = write fd
    mov     rax, SYS_WRITE          ; write "PIPE" to the write end
    mov     edi, r13d
    lea     rsi, [rel m_pipedata]
    mov     rdx, 4
    syscall
    cmp     rax, 4
    jne     .pipe_done
    mov     rax, SYS_READ           ; read it back from the read end
    mov     edi, r12d
    lea     rsi, [rbx+16]
    mov     rdx, 4
    syscall
    cmp     rax, 4
    jne     .pipe_done
    mov     eax, [rbx+16]
    cmp     eax, [rel m_pipedata]   ; "PIPE" round-tripped?
    jne     .pipe_done
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_pipeok]
    mov     rdx, m_pipeok_len
    syscall
    ; dup2(read fd -> 30), then write via r13 and read via fd 30
    mov     rax, SYS_DUP2
    mov     edi, r12d
    mov     esi, 30
    syscall
    cmp     rax, 30
    jne     .pipe_done
    mov     rax, SYS_WRITE
    mov     edi, r13d
    lea     rsi, [rel m_pipedata]
    mov     rdx, 4
    syscall
    mov     rax, SYS_READ
    mov     edi, 30
    lea     rsi, [rbx+16]
    mov     rdx, 4
    syscall
    cmp     rax, 4
    jne     .pipe_done
    mov     eax, [rbx+16]
    cmp     eax, [rel m_pipedata]
    jne     .pipe_done
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_dup2ok]
    mov     rdx, m_dup2ok_len
    syscall
.pipe_done:
    add     rsp, 32

    ; ---- PROC-B: epoll — watch a pipe read-end for EPOLLIN ------------------
    sub     rsp, 48                 ; [rbx]=fds(8) [rbx+8]=ev(12) [rbx+24]=outev(12)
    mov     rbx, rsp
    mov     rax, SYS_PIPE
    mov     rdi, rbx
    syscall
    test    rax, rax
    jnz     .epoll_done
    mov     r12d, [rbx]             ; read fd
    mov     r13d, [rbx+4]           ; write fd
    mov     rax, SYS_EPOLL_CREATE
    mov     rdi, 1
    syscall
    test    rax, rax
    js      .epoll_done
    mov     r14, rax                ; epfd
    mov     dword [rbx+8], 1        ; ev.events = EPOLLIN
    mov     rax, r12
    mov     [rbx+12], rax           ; ev.data = read fd
    mov     rax, SYS_EPOLL_CTL
    mov     rdi, r14
    mov     rsi, 1                  ; EPOLL_CTL_ADD
    mov     edx, r12d
    lea     r10, [rbx+8]
    syscall
    test    rax, rax
    jnz     .epoll_done
    ; before data: wait should report 0 ready
    mov     rax, SYS_EPOLL_WAIT
    mov     rdi, r14
    lea     rsi, [rbx+24]
    mov     rdx, 1
    xor     r10, r10
    syscall
    test    rax, rax
    jnz     .epoll_done             ; expected 0 (empty pipe)
    ; write one byte, then wait should report 1 ready with EPOLLIN
    mov     rax, SYS_WRITE
    mov     edi, r13d
    lea     rsi, [rel m_pipedata]
    mov     rdx, 1
    syscall
    mov     rax, SYS_EPOLL_WAIT
    mov     rdi, r14
    lea     rsi, [rbx+24]
    mov     rdx, 1
    xor     r10, r10
    syscall
    cmp     rax, 1
    jne     .epoll_done
    mov     eax, [rbx+24]           ; outev.events
    cmp     eax, 1                  ; EPOLLIN
    jne     .epoll_done
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_epollok]
    mov     rdx, m_epollok_len
    syscall
.epoll_done:
    add     rsp, 48

    ; ---- PROC-C: signals — catch SIGUSR1 sent to self ----------------------
    mov     rax, SYS_SIGACTION       ; install the SIGUSR1 handler
    mov     rdi, SIGUSR1
    lea     rsi, [rel sig_handler]
    syscall
    test    rax, rax
    jnz     .sig_done
    mov     rax, SYS_GETPID
    syscall
    mov     rbx, rax                 ; rbx = own pid (callee-saved)
    mov     rax, SYS_KILL            ; kill(self, SIGUSR1) -> sets pending
    mov     rdi, rbx
    mov     rsi, SIGUSR1
    syscall
    ; busy-loop so a timer IRQ fires in ring 3 and delivers the signal; the
    ; handler runs mid-loop, prints, and sigreturns (restoring rcx), then the
    ; loop runs to completion.
    mov     rcx, 0x10000000
.sig_spin:
    dec     rcx
    jnz     .sig_spin
.sig_done:

    ; ---- PROC-E: io_uring — batch WRITE then READ on a pipe ----------------
    mov     rax, SYS_IO_URING_SETUP
    mov     rdi, 8
    syscall
    test    rax, rax
    js      .uring_done
    mov     r14, rax                 ; ring user VA
    sub     rsp, 32
    mov     rbx, rsp                 ; [rbx]=fds(8) [rbx+8]=rbuf
    mov     rax, SYS_PIPE
    mov     rdi, rbx
    syscall
    test    rax, rax
    jnz     .uring_done2
    mov     r12d, [rbx]              ; read fd
    mov     r13d, [rbx+4]            ; write fd
    ; ---- DDR-1108: ENTER must REFUSE an unaligned ring VA ------------------
    ; sys_io_uring_enter validated only the CONTAINING page (va & ~0xFFF) and
    ; then formed its kernel pointer at phys + (va & 0xFFF), so a caller chose
    ; the intra-page offset of an 416-byte struct the kernel reads 8 SQEs from
    ; and writes 8 CQEs to. Any (va & 0xFFF) > 3680 leaves the frame entirely.
    ;
    ; This arm uses K=96 -- a SAFE, IN-PAGE offset. It proves the MECHANISM (an
    ; arbitrary caller offset is honoured); the out-of-page case follows from
    ; the same arithmetic and is deliberately NOT attempted here, because a
    ; probe that corrupted an unrelated physical frame would fail its own gate
    ; for reasons nobody could attribute. See DDR-1108 sec.7.
    ;
    ; At K=96 the struct's sqes[0] lands at page +128 and cqes[0] at +384, both
    ; disjoint from the bytes the aligned arm below uses (+32..+96, +296, +312).
    ; The poison SQE writes 2 bytes to the SAME pipe: if the refused call had
    ; executed, the pipe would hold "XXURING" and the aligned arm's 5-byte read
    ; would return "XXURI" and fail its OWN byte comparison -- so that existing
    ; assertion is the did-not-execute half, and no new arm has to be kept in
    ; step (DDR-1108 sec.6).
    mov     byte [r14+128], 1       ; poison SQE: opcode = OP_WRITE
    mov     dword [r14+132], r13d   ; fd = the write end
    lea     rax, [rel m_uringpois]
    mov     [r14+136], rax          ; addr
    mov     dword [r14+144], 2      ; len = 2 ("XX")
    mov     rax, SYS_IO_URING_ENTER
    lea     rdi, [r14+96]           ; UNALIGNED ring VA
    mov     rsi, 1
    syscall
    ; EXACTLY -EINVAL (-22), never "< 0" (DDR-1044): a range check failing for
    ; its own reason returns -EFAULT (-14) and would satisfy "< 0" while this
    ; guard was absent. And the branch below skips only the PRINT, never the
    ; arms after it -- DDR-1089 sec.6.1, because the aligned arm below IS the
    ; did-not-execute half of this one and jumping out would delete it.
    cmp     rax, -22
    jne     .uring_align_done
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_uringalign]
    mov     rdx, m_uringalign_len
    syscall
.uring_align_done:

    ; SQE[0] @ +32 : WRITE "URING"(5) to the write end
    mov     byte [r14+32], 1        ; opcode = OP_WRITE
    mov     dword [r14+36], r13d    ; fd
    lea     rax, [rel m_uringdata]
    mov     [r14+40], rax           ; addr
    mov     dword [r14+48], 5       ; len
    ; SQE[1] @ +64 : READ 5 bytes from the read end into rbuf
    mov     byte [r14+64], 0        ; opcode = OP_READ
    mov     dword [r14+68], r12d    ; fd
    lea     rax, [rbx+8]
    mov     [r14+72], rax           ; addr
    mov     dword [r14+80], 5       ; len
    mov     rax, SYS_IO_URING_ENTER
    mov     rdi, r14
    mov     rsi, 2
    syscall
    cmp     rax, 2
    jne     .uring_done2
    cmp     dword [r14+296], 5      ; cqe[0].res (write) == 5
    jne     .uring_done2
    cmp     dword [r14+312], 5      ; cqe[1].res (read) == 5
    jne     .uring_done2
    mov     eax, [rbx+8]            ; rbuf[0..3] == "URIN"
    cmp     eax, [rel m_uringdata]
    jne     .uring_done2
    mov     al, [rbx+12]           ; rbuf[4] == 'G'
    cmp     al, [rel m_uringdata+4]
    jne     .uring_done2
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_uringok]
    mov     rdx, m_uringok_len
    syscall
.uring_done2:
    add     rsp, 32
.uring_done:

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

; SIGUSR1 handler — entered via signal delivery (kernel sets RIP here, RDI=signum).
; Prints, then sigreturns (restoring the interrupted frame). Never reached by
; normal control flow.
sig_handler:
    mov     rax, SYS_WRITE
    mov     rdi, STDOUT
    lea     rsi, [rel m_sigusr1]
    mov     rdx, m_sigusr1_len
    syscall
    mov     rax, SYS_SIGRETURN
    syscall
.sig_handler_hang:
    jmp     .sig_handler_hang      ; sigreturn does not return; guard anyway

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
m_mmapfd:    db "SYSMMAP FD REJECTED", 10
m_mmapfd_len: equ $ - m_mmapfd
; ---- DDR-1112 literals ------------------------------------------------
; PLACED AFTER m_mmapfd_len ON PURPOSE. An earlier draft inserted them
; between `m_mmapfd:` and its `equ $ - m_mmapfd`, which made that length
; ~120 bytes instead of 20 — so the FD arm's write DUMPED THIS WHOLE BLOCK
; to the console and the gate matched "SYSMMAP FILE OK" out of .rodata
; rather than from the feature working. Mutant M1 (feature deleted) then
; PASSED. A length computed at a distance from its string is a live
; vacuity hazard: keep every `equ $ - x` adjacent to x.
m_mmapnd:    db "SYSMMAP ND REJECTED", 10
m_mmapnd_len: equ $ - m_mmapnd
m_mmapfile:  db "SYSMMAP FILE OK", 10
m_mmapfile_len: equ $ - m_mmapfile
m_mmapcur:   db "SYSMMAP CURSOR OK", 10
m_mmapcur_len: equ $ - m_mmapcur
m_mmapoffrd: db "SYSMMAP FILEOFF OK", 10
m_mmapoffrd_len: equ $ - m_mmapoffrd
p_bigpat:    db "/BIGPAT.BIN", 0
; The exact 25 bytes of /HELLO.TXT (build/hello.txt). A kernel handing
; back zero pages cannot produce them.
m_hellobytes: db "PRADYOS filesystem works!"
m_hellobytes_len: equ $ - m_hellobytes
m_mmapoff:   db "SYSMMAP OFF REJECTED", 10
m_mmapoff_len: equ $ - m_mmapoff
m_munmap:    db "SYSMUNMAP OK", 10
m_munmap_len: equ $ - m_munmap
p_exec:      db "/EXECTEST.ELF", 0
m_execbug:   db "EXECVE: post-exec (BUG)", 10
m_execbug_len: equ $ - m_execbug
m_forkp:     db "FORK: parent pid= child fork OK", 10
m_forkp_len: equ $ - m_forkp
m_forkc:     db "FORK: child running pid= OK", 10
m_forkc_len: equ $ - m_forkc
m_forkerr:   db "FORK: ERROR", 10
m_forkerr_len: equ $ - m_forkerr
m_wait:      db "WAIT: child exited status=42", 10
m_wait_len:  equ $ - m_wait
m_vdsop:     db "VDSO: clock ns="
m_vdsop_len: equ $ - m_vdsop
m_pipedata:  db "PIPE"
m_pipeok:    db "PIPE: roundtrip OK", 10
m_pipeok_len: equ $ - m_pipeok
m_dup2ok:    db "PIPE: dup2 OK", 10
m_dup2ok_len: equ $ - m_dup2ok
m_epollok:   db "EPOLL: pipe event OK", 10
m_epollok_len: equ $ - m_epollok
m_sigusr1:   db "SIGNAL: SIGUSR1 caught", 10
m_sigusr1_len: equ $ - m_sigusr1
m_uringdata: db "URING"
m_uringpois: db "XX"
m_uringalign:   db "IO_URING: unaligned ring VA refused", 10
m_uringalign_len: equ $ - m_uringalign
m_uringok:   db "IO_URING: batch read OK", 10
m_uringok_len: equ $ - m_uringok
