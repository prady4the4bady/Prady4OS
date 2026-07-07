# PRADYOS Build Status

Updated after every phase. Status legend:
🔴 NOT BUILT · 🟡 IN PROGRESS · 🟢 COMPLETE · ⚠️ BROKEN

Target ISAs, platform profiles (x86_64 / ARM64 incl. Grace Blackwell / Apple /
RISC-V64), the 8-layer map, and the branch strategy live in
[platform_profiles.md](platform_profiles.md).

**Current phase:** 5 — userspace. Layer 2 (NEXUS kernel core) COMPLETE: boot →
long mode → ring-0 C kernel; own GDT/IDT; CPU exceptions w/ panic dumps; legacy
PIC + PIT @100Hz; buddy PMM (ADR-003) + SLAB heap (leak-free); higher-half
kernel @0xFFFFFFFF80000000 + kernel-owned VMM (ADR-007); preemptive round-robin
scheduler + asm context switch (~107 ns); NCS capability system (ADR-009, 11/11
tests); full NIA IPC fabric (sync + async ring + broadcast, ADR-010/011);
syscalls (NSI) + ring-3 user mode (ADR-012, SYSCALL/SYSRET, TSS, cap-gated).
**Phase 3 (Layer 3 drivers) COMPLETE:** ACPI parser + PCIe MMCONFIG enumeration
(ADR-013, 7 devices on q35); reusable modern-1.0 virtio transport + generic
block layer + interrupt-driven virtio-blk (ADR-014); CMOS RTC (ADR-020).
**Phase 4 (Layer 4 FS) COMPLETE:** capability-gated VFS with a mount table +
per-mount context (ADR-017, FAT32/SFS/ext4 side-by-side). **FAT32 read-write**
(ADR-015): nested paths (4a/4b), create/write/unlink with all-or-nothing alloc +
read-back verify (4c), VFAT long-name read + RTC timestamps (4j, ADR-020).
**SOVEREIGN FS (SFS) engine** (ADR-018): in-kernel format/mount (4d), CoW B+tree
create/lookup/open/readdir (4e), file extents (4f), journal + atomic transactions
with crash replay (4g), snapshots/version isolation (4h), per-extent inline LZ4 +
metadata tags (4i). **ext4 read-only** (ADR-019, 4j). Shared kernel state
(PMM/console/scheduler) made preemption-safe (ADR-016). **Layer 4 completion gate
PASSED** — all 5 gates green (smoke, smoke-fs, smoke-fs-rw, smoke-fs-sfs-rw,
smoke-fs-ext4) on CI. **Phase 5 (Layer 5 userspace) IN PROGRESS — slice 5a
(static ELF loader + W^X) COMPLETE** (ADR-021): per-process address spaces
(`vmm_new_address_space`/`vmm_map_in`/`vmm_destroy_address_space`, kernel
higher-half shared, user range = PML4 slot 1); per-process CR3 switched on
context switch; **EFER.NXE enabled (CPUID-gated)** so VMM_NX/W^X is honored;
ELF64 loader (`kernel/exec/elf.c`) maps each PT_LOAD with permissions derived
from p_flags (text RX, rodata R-NX, data RW-NX; W+X rejected), 8 MiB RW-NX stack
+ unmapped guard page, SysV argv/envp/auxv frame; a ring-3 fault is turned into a
clean process kill (no kernel panic). The test program is written to SFS and
**loaded back from SFS**; it prints `HELLO FROM RING-3` and exits via sys_exit. A
W^X negative regression (write to RX text → #PF err=0x7 → clean kill, kernel
survives) is asserted. New gate `smoke-user` PASS; all 5 prior FS gates still
green. Next: 5b (~50 POSIX syscalls). Build is warning-clean and `-Werror`
enforced (C + NASM). Deferred: 3-lane NAS, APIC, kernel-self W^X, COW fork,
process reaping/AS-teardown on exit, dynamic linking, SFS free-space-tree/GC,
ext4 write (see DEFERRED). Architecture confirmed against the user's Layer 1–6
boards; realistic perf targets adopted (context switch ≤ 1.5 µs, syscall ≤ 250 ns).
**5b design recorded (ADR-022 = DDR-5b):** validated user-pointer copy contract
(`copyin`/`copyout`/`copyinstr` → EFAULT, never CPL-0 panic), NSI-v2 table/ABI
(6-arg, own number space, fd↔capability bridge), 9 syscall clusters in build
order, fork = copy-all-pages (COW deferred to a future ADR), mmap MAP_ANON
baseline. **Slice 5b-2 (uaccess primitives) COMPLETE:** `kernel/mm/uaccess.c`
(`copyin`/`copyout`/`copyinstr`) on `vmm_user_range_ok` — walks the calling
process's page tables WITHOUT dereferencing the user address, so a wild pointer
or a read-only-page write returns `-EFAULT` and the kernel never #PFs at CPL 0
(W^X upheld on the copy path). New gate `smoke-uaccess` PASS (good page, wild
ptr → EFAULT, RO-page write → EFAULT, valid string). **Slice 5b-3
(sys_read/sys_write + per-process fd table) COMPLETE:** per-process `fd_table`
(`kernel/proc/fd.c`, FD_MAX=64) embedded in the TCB, fds 0/1/2 pre-wired to the
console for user threads; `sys_write` (`kernel/syscall/sys_io.c`) copies the user
buffer via `copyin` and writes to the console (-EBADF on bad fd, -EFAULT on bad
buffer, kernel survives); `sys_read` stubbed -ENOSYS until slice 4; NSI-v2 table
grown to 64, unknown call → -ENOSYS; `SYS_READ=5`/`SYS_WRITE=6`. New ring-3
`user/systest.asm` (grows per slice) drives these; new gate `smoke-sysio` PASS
(SYSWRITE OK / EBADF / EFAULT). **Slice 5b-4 (sys_open/sys_close/sys_fstat)
COMPLETE:** `kernel/syscall/sys_file.c` bridges the POSIX fd API to the
capability-gated VFS — `sys_open` copyinstr's the path, resolves it against the
process root mount with the process FS capability (both granted at load by
`elf_load`; root = the stable FAT32 mount, since the SFS mount is reformatted by
the destructive self-tests), allocates an fd, and records the open file + cap;
`sys_read` now reads VFS files through `copyout`; `sys_close` frees the slot;
`sys_fstat` fills a Linux-x86-64-layout `struct stat` (`kernel/include/stat.h`)
via copyout. `SYS_OPEN=7`/`SYS_CLOSE=8`/`SYS_FSTAT=9`. New gate `smoke-sysfile`
PASS (open/fstat/read/close + ENOENT). **Slice 5b-5 (sys_lseek/sys_getcwd/getpid)
COMPLETE:** `kernel/syscall/sys_proc.c` — `sys_lseek` (SEEK_SET/CUR/END on a VFS
fd, -ESPIPE for console, -EINVAL on negative result) and `sys_getcwd` ("/" via
copyout, -ERANGE if the buffer is too small); `sys_getpid` already existed
(SYS_GETPID=2). `SYS_LSEEK=10`/`SYS_GETCWD=11`. New gate `smoke-sysproc` PASS
(getpid>0, getcwd="/", lseek-then-read returns the byte at the new offset).
**Slice 5b-6 (sys_mmap MAP_ANON RW+NX baseline) COMPLETE:**
`kernel/syscall/sys_mmap.c` — `sys_mmap` maps anonymous private RW+NX pages
(ptnode-allocated, like the ELF loader) into the user mmap arena
([544 GiB, 572 GiB), below the stack); **PROT_EXEC is rejected (-EINVAL, W^X)**;
`sys_munmap` unmaps + frees the region (via new `vmm_resolve` va→phys) so a
re-mmap of the same hint succeeds. Per-process `vm_area` table + bump pointer in
the TCB. `SYS_MMAP=12`/`SYS_MUNMAP=13`. The 6-arg ABI widening is **deferred**
(every 5b call fits in ≤4 args; anon mmap ignores fd/offset — see ADR-022 note).
New gate `smoke-sysmmap` PASS. **Slice 5b-7 (sys_execve) COMPLETE:**
`kernel/syscall/sys_exec.c` — `sys_execve` replaces the calling process's image.
The ELF loader core was refactored: `elf_build_image` (image → fresh W^X address
space + entry + initial user RSP, no thread) is now shared by `elf_load` (which
adds the thread) and `sys_execve`. execve `copyinstr`'s the path, opens + reads
the target from the process root FS (the stable FAT32 mount) into a kernel
buffer, builds the new address space, then — past the point of no return, under
the SYSCALL's cleared IF (no preemption) — closes non-stdio fds (0/1/2 survive),
points the TCB at the new CR3/entry/RSP, resets the anon-mmap arena, switches CR3
(kernel stacks are mapped in every AS), tears down the **old** address space, and
drops to ring 3 via `enter_user_mode` (so it never returns on success). On any
pre-commit failure the old image keeps running with a negative errno (new
`ENOEXEC`). The kernel places `/EXECTEST.ELF` (a second embedded static ELF,
`user/exectest.asm`) on the FAT32 root each boot; `user/systest.asm` then
`SYS_EXECVE`'s it — the new image's `EXECVE: new image running` sentinel must
appear and systest's post-execve `(BUG)` line must NOT. `boot_test.sh` gained a
`FORBIDDEN_SENTINEL` check for that absence. `SYS_EXECVE=14`. argv/envp
marshalling is **deferred** (baseline argv[0] = path); the exec image must fit
the 8 KiB user-ELF budget (matches the Makefile cap + SFS bootstrap buffer). New
gate `smoke-sysexec` PASS (14 syscall/FS+user gates total). Code graph: 89 files
/ 937 symbols. **Slice 5b-8 (sys_fork, copy-all-pages) COMPLETE:** `sys_fork`
(`SYS_FORK=15`, `kernel/syscall/sys_fork.c`) duplicates the calling process.
`kernel/mm/vmm_fork.c` (`vmm_fork_address_space_copy`) deep-copies every present
USER leaf page into a fresh AS (kernel top-level entries shared, not copied;
frames `ptnode_alloc`'d so `vmm_destroy_address_space` reclaims them) — the
ADR-022 baseline; COW is the IMP-D follow-on. `sched_create_user_clone`
(`kernel/proc/sched.c`) builds the child TCB, copies the capability table
(`cap_fork`, an exact slot copy so parent handles stay valid) and fd table
(`fd_clone`, which deep-copies each `vfs_file` so parent/child close
independently — no shared-pointer double-free), and enqueues it; `sched_destroy`
unlinks a never-run/reaped thread and frees its kstack/caps/fds/TCB. Per the
confirmed architectural fact (no stored SYSCALL trap-frame), the child resumes
via `enter_user_mode` at the caller's user RIP/RSP — captured at SYSCALL entry in
new globals `syscall_user_rip`/`syscall_user_rsp` — with RAX=0 (the parent gets
the child pid via the normal SYSRET path). New TCB fields `parent_pid`,
`fork_retval`. Registers other than RAX are not replicated in the child (baseline
limitation, documented). New gate `smoke-sysfork` PASS (15 gates total). Kernel
112,404 B (<256 KiB). Code graph: 93 files / 962 symbols. **Slice 5b-9 (sys_wait4
+ process reaping) COMPLETE:** `sys_wait4` (`SYS_WAIT4=16`,
`kernel/syscall/sys_wait.c`) reaps a child. `sched_exit` now takes the exit
status and leaves the thread in a new `THREAD_ZOMBIE` state holding it (waking any
parent blocked in wait4) instead of leaking as `THREAD_DONE`; `sys_exit` passes
the user code, the #PF user-kill path passes -1. `sys_wait4(pid, *status,
options)` finds the caller's child (parent_pid match), blocks until it is a zombie
(unless WNOHANG → -EAGAIN), copyout's the raw exit status, then reclaims the
child's AS + TCB (`vmm_destroy_address_space` + `sched_destroy`). A low-priority
`reaper` kernel thread (spawned in `sched_demo`) sweeps orphaned zombies (exited
procs whose parent is gone and which no wait4 is collecting), bounding the leak —
this also reclaims the hello/wxviol/exectest/fork-child address spaces that
previously leaked. New TCB fields `exit_status`, `waiter`; new errno `EAGAIN`.
Also fixed a latent systest fork bug: the post-`syscall` branches lacked a `test
rax, rax` (SYSRET restores user RFLAGS), so they read stale flags. New gate
`smoke-syswait` PASS (16 gates total). Kernel 113,572 B. Code graph: 95 files /
975 symbols. **IMP-A (Spectre/Meltdown MSR mitigations) COMPLETE:**
`kernel/arch/x86_64/cpu_mitigations.c` — `cpu_mitigations_init` (called in kmain
after `idt_init`) probes CPUID.7.0:EDX and, where the CPU advertises them, sets
IBRS/STIBP/SSBD in IA32_SPEC_CTRL (0x48) and fires an IBPB barrier
(IA32_PRED_CMD 0x49); each wrmsr is gated on its CPUID bit so an unsupporting CPU
is never written (no #GP). Inline asm uses the split-operand wrmsr + explicit
cpuid outputs. QEMU TCG advertises none → prints `[cpu] mitigations: IBRS=0
STIBP=0 SSBD=0 IBPB=0` (zero CI impact). New `kernel/arch/x86_64` dir
(`-Ikernel/arch/x86_64`). New gate `smoke-mitigations` PASS (17 gates total).
Kernel 114,116 B. Code graph: 97 files / 981 symbols. **IMP-B (PMM poison + heap
canary) COMPLETE:** new Makefile `KASAN ?= 1` (default on → `-DKASAN=1`).
`kernel/mm/pmm.c` fills every freed frame with `0xDEADBEEFDEADBEEF` (the free-list
link at offset 0 is re-set afterwards) and prints `[pmm] poison enabled`.
`kernel/mm/kheap.c` arms an 8-byte `0xFEEDFACEFEEDFACE` canary at offset 8 of
every free slab object (written in both `cache_grow` and `cache_free`, every size
class is ≥16 B) and verifies it on each `cache_alloc` → `KHEAP PANIC` on a
use-after-free write. Because KASAN is the default, all prior gates are now
implicit poison/canary regression tests. New gate `smoke-pmm-poison` PASS (18
gates total). Kernel 114,356 B. Code graph: 97 files / 983 symbols. **IMP-C (vDSO
clock page) COMPLETE:** `kernel/vdso/vdso_page.c` allocates one shared frame;
`vdso_init` (kmain, after pmm) zeroes it and the PIT IRQ (`idt.c`, null-guarded)
advances `wall_time_ns` 10 ms/tick under a seqlock. `vdso_map_user` (called from
`elf_build_image`) maps the frame **read-only + NX** into every user AS at
`VDSO_USER_VA` (0x7FFFFFF00000), so ring 3 reads the clock with a single aligned
`mov` — no syscall (`systest` prints `VDSO: clock ns=<N>`, only when non-zero).
W^X holds: kernel view is RW-not-X, user view is R-only. New `PTE_SW_SHARED` bit
(vmm.h bit 10) marks the shared frame so `vmm_destroy_address_space` (free_subtree)
never frees it and `vmm_fork` shares rather than copies it. The executable
callable reader (vdso_entry.asm) is deferred — a single u64 needs no seqlock on
the read side. New gate `smoke-vdso` PASS (19 gates total). Kernel 114,916 B. Code
graph: 99 files / 996 symbols. **IMP-D (copy-on-write fork) COMPLETE:** replaces
copy-all-pages. `kernel/mm/vmm_cow.c` (`vmm_fork_address_space_cow`) shares every
present user page with the child and reference-counts the frame
(`pmm_incref`), marking writable pages read-only + `PTE_SW_COW` (bit 9) in BOTH
spaces; the vDSO (`PTE_SW_SHARED`) is shared verbatim. The #PF handler
(`idt.c`) routes a ring-3 write fault (err 0x7) on a COW page to `vmm_cow_fault`,
which gives the writer a private copy when the frame is shared (refcount>1) or
just re-grants write when it is the sole owner; non-COW faults return -1 and fall
through to the existing kill path, so W^X (wxviol) still dies. PMM gained per-frame
refcounts (`pmm_incref`/`pmm_refcount_get`; `pmm_free_page` frees only at 0,
checked before the KASAN poison); the 512 KiB refcount table is allocated FROM the
pool at `pmm_init` (too large for the kernel's low-memory BSS — a 512 KiB BSS
array overran the 0xA0000 VGA boundary and hung the boot). `vmm_fork.c`/`.h`
removed; `sys_fork` now calls the COW path. In-kernel `cow_selftest` proves
isolation; the ring-3 #PF COW path is covered by smoke-sysfork/smoke-syswait
(the parent writes its stack after fork). New gate `smoke-cowfork` PASS (20 gates
total). Kernel 117,108 B. Code graph: 99 files / 1010 symbols. Phase 5b + IMP-A..D
complete. **NET-A (virtio-net driver) COMPLETE:** `kernel/drivers/net/virtio_net.c`
reuses the modern virtio-pci transport — `virtio_pci_attach`/`negotiate`(VERSION_1
+ F_MAC)/`setup_queue` for RX(0) and TX(1), reads the MAC from device config, arms
RX with `netbuf` pool buffers, registers the shared INTx handler, and DRIVER_OK.
It then transmits one broadcast Ethernet frame (virtio_net_hdr + ARP) and reaps
its TX completion off the used ring — a reliable functional check (QEMU completes
TX regardless of where the packet goes). `kernel/drivers/net/netbuf.c` is a fixed
LIFO pool of page DMA buffers (no alloc in the IRQ path). Detected in kmain's PCIe
scan (vendor 0x1AF4, class 0x02). Sentinels `[net] virtio-net up MAC=...` and
`[net] virtio-net TX OK`. Root-cause fix folded in: `irq_register`/`idt.c` now
keep a small per-line handler CHAIN (idempotent) instead of one handler per IRQ —
PCI INTx is shared, and the single-slot registry had let virtio-net clobber the
virtio-blk handler, hanging block I/O (and the FS/user gates). A socket API and true peer loopback (needs a tap/socket
netdev, not QEMU SLIRP) are deferred to NET-B. New gate `smoke-net` PASS (21 gates
total). Kernel 119,156 B. Code graph: 103 files / 1040 symbols. **PROC-A (pipe +
dup2) COMPLETE:** `kernel/proc/pipe.c` — a pipe is a 4 KiB byte ring (power-of-two
mask, `head-tail` = bytes buffered) shared by a read-end and write-end fd,
reference-counted by the fds that name it (pipe/dup2/fork). `SYS_PIPE=17` installs
both fds and copyouts `fds[2]`; `SYS_DUP2=18` duplicates onto a chosen fd (shares
the pipe via `pipe_incref`, or deep-copies an FD_VFS). New `FD_PIPE` kind +
`pipe` field in `fd_entry`; `fd_free` calls `pipe_close` (frees ring at refcount
0), `fd_clone` increfs so a forked child shares the pipe. `sys_read`/`sys_write`
route `FD_PIPE` to `pipe_read`/`pipe_write` (non-blocking baseline; read of empty
returns 0). systest round-trips "PIPE" through a pipe and through a dup2'd read
end. New gate `smoke-syspipe` PASS (22 gates total). Kernel 122,276 B. (Graph
counts carried — graph tool deps need an `npm ci` refresh this session;
+`kernel/proc/pipe.{c,h}` → 105 files.) **PROC-B (epoll skeleton) COMPLETE:**
`kernel/proc/epoll.c` — `SYS_EPOLL_CREATE=19`/`_CTL=20`/`_WAIT=21`. An epoll
instance is an `FD_EPOLL` fd with a fixed 64-entry interest table; `epoll_ctl`
ADD/MOD/DEL'es `(fd, events, data)` (packed 12-byte `epoll_event`), `epoll_wait`
non-blockingly polls readiness and copies out ready events. Baseline readiness:
`EPOLLIN` on a pipe read-end iff the ring has bytes (`pipe_has_data`). epoll fds
are freed by `fd_free` (sole owner) and NOT inherited across fork. systest watches
a pipe read-end: 0 ready when empty, 1 ready (EPOLLIN) after a write. New errno
`EEXIST`. New gate `smoke-sysepoll` PASS (23 gates total). **PROC-C (POSIX signals)
COMPLETE:** `kernel/proc/signal.c` — `SYS_SIGACTION=22` (install a ring-3 handler
VA per signal; SIGKILL uncatchable), `SYS_KILL=23` (set the target's pending bit),
`SYS_SIGRETURN=24`. `struct regs` moved to shared `kernel/include/regs.h`.
Delivery (`signal_deliver`, from idt.c's timer-IRQ return to ring 3): SIGKILL/
unhandled-SIGTERM → `sched_exit(-1)`; a caught signal snapshots the interrupted
frame into the TCB, redirects RIP→handler with RDI=signum, sets `sig_active`. The
handler ends with `sys_sigreturn`, which IRETQs back to the snapshot via
`signal_sigreturn` (usermode.asm) — a full GP+RIP+RSP+RFLAGS restore. New TCB
fields `sig_pending`/`sig_handlers[32]`/`sig_saved`/`sig_active`; new errno
`ESRCH`. systest installs a SIGUSR1 handler, kills itself, busy-loops so a timer
tick delivers it (`SIGNAL: SIGUSR1 caught`), then resumes. New gate
`smoke-syssignal` PASS (24 gates total). **PROC-E (io_uring batch) COMPLETE:**
`kernel/syscall/sys_io_uring.c` — `SYS_IO_URING_SETUP=25` maps one zeroed ring
page RW+NX into the process mmap arena and returns its user VA (header + 8 SQEs +
8 CQEs in one page); `SYS_IO_URING_ENTER=26` validates the ring VA
(`vmm_user_range_ok` over the whole page), resolves it to the shared frame, runs
the first `to_submit` SQEs (OP_READ/OP_WRITE on FD_PIPE / FD_VFS / console via
copyin/out), and posts one CQE each. systest batches a WRITE then a READ on a pipe
in a single enter and verifies both completions + the data (`IO_URING: batch read
OK`). New gate `smoke-sysiouring` PASS (25 gates total).

**Phase 5b COMPLETE** through PROC-A/B/C/E + all IMP-A..D.

**PROC-D (musl libc) IN PROGRESS — step 1/3 done (ADR-023).** Kernel foundation
for the musl port: `SYS_SET_TLS=27` programs `IA32_FS_BASE` for the calling
thread (validated to the user range; restored on switch-in from `tcb.fs_base`,
inherited across fork) — the thread pointer musl needs before `main`.
`SYS_WRITEV=28` gather-writes an iovec array via the validated copyin path
(refactored shared `fd_write_user` helper). The user-ELF budget `EXEC_MAX` is
raised 8 KiB → **256 KiB** (PMM-pool buffer in `sys_exec` + the SFS bootstrap
loader; Makefile size check follows) so musl binaries load. Ring-3 probe
`user/tlstest.asm` round-trips a value through `%fs:0` and gathers two iovecs to
fd 1; `smoke-user` greps `PRADYOS_TLS_OK WRITEV_OK`.

**PROC-D step 2/3 done.** The minimal musl subset builds to `build/musl/lib/`
(`libc.a` + `crt1.o`) via `make musl` / `tools/build_musl.sh` from the pinned
`third_party/musl` (v1.2.5) + `third_party/musl-overlay/`. The overlay: (1) a
generated `bits/syscall.h` that offsets every musl x86_64 number by +4096 (so any
unimplemented call is ≥ `MAX_SYSCALLS`=64 → `-ENOSYS`, never a mis-dispatch to an
unrelated NSI handler such as native `ioctl`=16 → `SYS_WAIT4`), then remaps the 7
we implement to their NSI numbers; (2) `__set_thread_area.s` issuing `SYS_SET_TLS`
instead of `arch_prctl`. New `user/user_c.ld` gives C programs the 3 W^X-clean
segments musl needs (RX text / R+NX rodata / RW+NX data+bss); C user code uses
`-mcmodel=large` (the 0x8000000000 base exceeds 32-bit relocs). CI now checks out
submodules and runs `make musl`.

**PROC-D COMPLETE (step 3/3 — all 8 gates green).** `user/cmusl.c` — the first
ring-3 C program on PRADYOS — links statically against the musl subset
(`crt1.o` + `libc.a`, `user/user_c.ld`), is written to SFS and loaded back, and
prints `PRADYOS_MUSL_OK v1.2.5 2026` via `printf` (→ `__stdio_write` →
`SYS_WRITEV` → serial). crt1 → `__libc_start_main` sets up the thread pointer
(overlay `__set_thread_area` → `SYS_SET_TLS`) and stdio. `cpu_enable_sse()`
(ADR-023 §D8) enables x87/SSE so the SysV varargs ABI works (printf's XMM
prologue `#UD`'d otherwise). `smoke-user` greps `PRADYOS_MUSL_OK`.
**FPU context-switch (ADR-023 §D8) — RESOLVED (5d):** `schedule()` now does eager
per-thread `FXSAVE`/`FXRSTOR` around `context_switch` (512-byte 16-aligned
`fpu_state` in the TCB; new threads/idle seeded from a clean `fninit`+MXCSR
template; forked children inherit). New gate **`smoke-fpu`**: two concurrent
ring-3 FPU users keep distinct XMM0 values over ~30M preemption-interleaved
iterations (OK=2, FAIL forbidden).

**5d pradyos-init (PID 1) — COMPLETE (ADR-023 §5d).** `user/init.c` is the first
long-lived ring-3 process: musl `printf` banner (`PRADYOS_INIT_OK …`), then a
`waitpid(-1, WNOHANG)` reap loop polling with `yield`, never exiting. It forks a
child (raw `SYS_FORK`) that `_exit(42)`s and reaps it (`init: reaped PID=N
exit=42`). Kernel support: `sys_wait4` gained `pid==-1` (any child); `sched_exit`
reparents a dying thread's children to init (PID 1 reaps the whole tree) and
**panics if init itself exits** ("init exited — system halted"). The existing
kernel reaper remains a backstop. init issues fork/wait4/yield by raw NSI number
(musl's wrappers pull in clone/cancellation plumbing not vendored). New gates
`smoke-fpu`, `smoke-init` (both in CI). NSI append-only (no new number — wait4=16
extended); ADR-021 W^X untouched.

**5e PRISM shell — COMPLETE (ADR-024).** `user/prism.c` is the first interactive
ring-3 shell: prints `PRISM_READY`, shows `prism> `, reads command lines from the
console, and dispatches builtins `help`/`echo`/`cat`/`run`/`ls`/`ps`/`exit`
(`echo`/`cat`/`run`/`exit` real; `ls`/`ps` minimal stubs pending
`SYS_GETDENTS`/process-table). Input via raw `SYS_READ`, output via musl `printf`.
Kernel support (no new NSI number): **console RX** — `sys_read(FD_CONSOLE)` now
works, fed by an **IRQ4-driven 256-byte ring** (`console_rx_init` in `console.c`)
so bulk input isn't lost during the cli-heavy boot; **full-register fork** — a
forked child now resumes with the parent's complete register frame (callee-saved
snapshot at syscall entry + RAX=0) via `signal_sigreturn`, fixing a documented
`sys_fork` limitation that left `rbp`/`rbx`/`r12-15` zeroed (broke non-inlined C
in the child / PRISM's `run`). PRISM is launched by the kernel via the proven SFS
`elf_load` path as **init's child** (init reaps it). New gate **`smoke-shell`**
(in CI): feeds `echo`/`help`/`exit` through a FIFO once `PRISM_READY` shows, and
checks the builtin output with no panic.
**Deferred (ADR-024):** init `fork`+`execve` **respawn** of PRISM — `execve` of a
large musl-C ELF from FAT32 corrupts (likely FAT32 multi-cluster read; SFS large
read is fine), a separate kernel fix; also `ls`/`ps` full impl, RX line
discipline/echo, pipes/redirection/quoting/job-control/scripting.
**NET-B (lwIP TCP/IP) COMPLETE:** lwIP 2.2.1 (pinned `third_party/lwip`, raw API,
`NO_SYS=1`, kmalloc-backed) is linked into the kernel via `build/lwip/liblwip.a`
(built `-w -nostdlibinc`) plus the first-party port `third_party/lwip-port/lwip_port.c`
(`-Werror`): allocator/rand/diag/assert/`sys_now` shims, the virtio-net⇄netif
bridge (`pradyos_linkoutput` TX, `pradyos_netif_rx` injects into `ethernet_input`),
static IP 10.0.2.15/24 GW 10.0.2.2, `net_init` wired into kmain and
`sys_check_timeouts`+`netif_poll_all` driven off the PIT tick (every 10 ticks).
Root-cause fix folded in: the NET-A RX path dropped frames — `virtio_net.c` now
recovers each buffer from `vq->desc[head].addr`, delivers the payload past the
12-byte `virtio_net_hdr` to the registered RX cb, re-arms the descriptor, and
frees TX chains (no leak). `net_init` runs IRQ-masked (lwIP is not reentrant under
`NO_SYS`). Gates: **`smoke-net-lo`** (UDP echo on 127.0.0.1 → `PRADYOS_NET_LO_OK`),
**`smoke-net`** (host→guest TCP echo on :8007 via QEMU hostfwd 18007→8007 →
`PRADYOS_NET_TCP_OK` + host receives the echo), **`smoke-net-fuzz`** (512
malformed/truncated frames + a 256-segment SYN flood to a closed port fed into the
RX path; kernel survives, no panic → `PRADYOS_NET_FUZZ_OK`). Stage2 kernel load
raised to 11×64 sectors (352 KiB) to fit liblwip.a. Serial print paths carry only
fixed sentinels — no kernel pointers leak to ring-3/network output. 29 gates total.
**Layer 6 (AETHER) COMPLETE:** the AI-native agent layer (ADR-026 + DDR-AETHER).
An untrusted ring-3 *agent* proposes consequential actions that the kernel
arbitrates. `kernel/aether/` holds a 256-entry action queue and a 4096-entry
append-only audit ring (both PMM-pool allocated, not BSS — the low-mem image cap),
a sovereign/manual mode flag (default sovereign auto-approves; process-spawn always
PENDING), a per-process 128 MiB memory cap with clean OOM kill, and a 60 syscall/s
rate limit; the kill *decision* (log+audit) is split from the kill *action*
(sched_exit) so every bound is unit-testable in-boot. 10 append-only NSI calls
(`SYS_GET_MODE`..`SYS_SET_MEM_LIMIT`, 29–38) in `kernel/syscall/sys_aether.c`,
all crossing copyin/copyout; authority is the kernel-set per-process flag
(`is_agent`/`is_sovereign`), never a user token (no self-escalation). `cap.h`
gained `CAP_SOVEREIGN`/`CAP_AGENT`. Userspace: `user/aether_daemon.c` (PID-2,
CAP_SOVEREIGN, spawns the agent) and `user/agent_base.c` (CAP_AGENT, submit→poll→
execute). Root cause fixed: `sched_create` left the appended TCB fields
uninitialised (kmalloc does not zero), so processes read `is_agent`!=0 and were
spuriously killed — now zeroed. Gates **`smoke-aether`** (daemon→agent→submit→
approve→execute→`PRADYOS_AGENT_DONE`), **`smoke-aether-queue`**
(`PRADYOS_AETHER_QUEUE_OK`), **`smoke-aether-sec`** (queue overflow, audit wrap,
OOM/rate kill, no self-escalation). 32 gates total.
**Ring-3 socket NSI (ADR-027) COMPLETE:** the bridge from ring-3 agents to the
in-kernel lwIP stack, without moving lwIP. `third_party/lwip-port` gains 8 proxy
sockets — each a lwIP TCP PCB + a 4 KiB PMM-pool RX ring; `psock_*` touch lwIP only
with interrupts masked (atomic vs the RX/PIT IRQ), and the recv callback applies
`ERR_MEM` backpressure so no byte is dropped. `kernel/syscall/sys_socket.c` adds 4
append-only syscalls (`SYS_SOCK_CONNECT`/`WRITE`/`READ`/`CLOSE`, 39–42), all
copyin/copyout; `SYS_SOCK_READ` parks on `sti;hlt;cli` with a tick timeout.
`user/agent_base.c` gains a live path (Ollama HTTP/1.1 `POST /api/generate` + a
hand-written JSON `"response"` extractor) printing `PRADYOS_AGENT_LIVE_OK` when
`AETHER_TEST_MODE=0`. Root cause fixed: the new code pushed the kernel image+BSS
past the boot page tables at physical `0x70000` (entry-stub BSS-zero wiped them →
triple fault); the 6 page tables moved to `0x300000`, above the kernel working
window and below the 16 MiB PMM floor, so a growing kernel can never overrun them.
Gate **`smoke-agent-live`** (`make smoke-agent-live [OLLAMA_HOST=a.b.c.d]`) is
developer-run (needs a real Ollama); CI stays test-mode (32 gates).
**Layer 7 (UI/UX) STARTED — mode binding (DDR-701):** the brief's Sovereign/Manual
toggle (§3) is bound to the kernel at the data/control layer. PRISM gains a `mode`
builtin (`mode get` → `SYS_GET_MODE`; `mode set` → `SYS_SET_MODE`, denied without
`CAP_SOVEREIGN` — the serial stand-in for Super+M). The AETHER daemon
(`CAP_SOVEREIGN`) runs a startup binding self-check (GET→SET manual→GET→SET
sovereign→GET → `PRADYOS_MODE_TOGGLE_OK`), ending sovereign. Gate **`smoke-mode`**
in CI (33 gates). **BLOCKER (honest):** the Layer-7 *visual* compositor (brief §12
7a–7j: wlroots/Wayland, OKLab transitions, glass shell) is blocked on a **VirtIO-GPU
framebuffer + modeset driver**, which PRADYOS lacks and which needs its own ADR +
slice sequence (a Layer-7 "slice 0"); only the toggle's kernel binding is built so
far, by design.
**Layer-7 slice 0 — VirtIO-GPU framebuffer (ADR-028) COMPLETE:** the display-output
prerequisite is in. `kernel/drivers/gpu/virtio_gpu.c` brings up scanout 0 over the
2D control queue (GET_DISPLAY_INFO → RESOURCE_CREATE_2D → ATTACH_BACKING →
SET_SCANOUT → TRANSFER_TO_HOST_2D → RESOURCE_FLUSH) and presents a linear BGRA
framebuffer from the PMM pool (phys==virt identity-mapped DMA), reusing the shared
virtio_pci/virtq transport; dispatched from kmain's PCIe scan (vendor 0x1AF4 class
0x03). Bring-up waits on the used ring with `HLT` (not a busy-spin, which starves
QEMU's TCG device backend) and suppresses/acks the shared INTx. Gate **`smoke-gpu`**
boots `-device virtio-gpu-pci` (added to `boot_test.sh` only under `QEMU_GPU=1`) and
greps `PRADYOS_GPU_FB_OK 1024x768` — verifiable headless.
**Ring-3 framebuffer surface (DDR-702) COMPLETE:** userspace can now draw to the
screen. Three syscalls (43–45): `SYS_FB_INFO` (geometry), `SYS_FB_MAP` (maps the
GPU front buffer into the caller at `0x8700000000`, `VMM_USER|RW|NX`),
`SYS_FB_FLUSH` (present). `virtio_gpu_present()` + a refactored `gpu_cmd` that
saves/restores RFLAGS and waits `sti;hlt;cli` make the control-queue wait work in
syscall context (IF=0) as well as at boot. `user/fbtest.c` maps the FB, draws, and
flushes → gate **`smoke-fb`** (`PRADYOS_FB_DRAW_OK`); no-GPU boots degrade to
`-ENODEV`. Stage 2 kernel load raised 11→16 chunks (512 KiB; safe — page tables at
`0x300000`, 1 MiB disk image). `smoke-gpu` is now also in CI.
**PS/2 keyboard input → ring 3 (DDR-703) COMPLETE:** the IRQ1 handler (was a
scancode-printing stub) now feeds `ps2kbd_isr` — read 0x60, track Shift, translate
scancode-set-1 → ASCII into a 256-byte ring; `SYS_INPUT_POLL` (46) drains it to
ring 3 (non-blocking). `user/inputtest.c` + gate **`smoke-input`** inject real keys
via QEMU's HMP `sendkey` (so the i8042 raises IRQ1 — genuine hardware path) and
confirm the byte reaches ring 3 (`PRADYOS_INPUT_OK a`). With DDR-702, ring 3 can
now both draw to the screen and read the keyboard.
**In-house sovereign-desktop compositor (DDR-704) COMPLETE:** `user/compositor.c`
— a single full-screen ring-3 process (CAP_SOVEREIGN), **not wlroots/Wayland** —
maps the GPU framebuffer (`SYS_FB_MAP`), renders the current mode's desktop
(background + accent bar + an embedded-8×8-font label: dark/purple SOVEREIGN,
light/teal MANUAL), and runs a keyboard loop (`SYS_INPUT_POLL`): `s`/`m` flip the
mode via `SYS_SET_MODE` (re-render + confirm), `q` exits. It is the **sole**
framebuffer consumer (fbtest folded in — two FB presenters contended on the GPU
control queue), emitting `PRADYOS_FB_DRAW_OK` + `PRADYOS_COMPOSITOR_OK` on the
first frame. Gate **`smoke-compositor`** boots with the GPU, then injects `m`/`s`
via QEMU `sendkey` (real IRQ1); the keyboard-driven sovereign→manual→sovereign
round-trip ends in `PRADYOS_COMPOSITOR_MODE SOVEREIGN`, proving keyboard → mode →
framebuffer end to end.
**virtio-input pointer (DDR-705) COMPLETE:** `kernel/drivers/input/virtio_input.c`
— a virtio-tablet (absolute) driver over the virtio-pci/virtq transport; the IRQ
handler folds `virtio_input_event`s into a current `{abs_x, abs_y, buttons}` state
and re-arms the eventq buffers. `SYS_MOUSE_POLL` (47) returns the state mapped to
screen pixels (non-consuming). The compositor polls it and, on a button-down,
draws a cursor + prints `PRADYOS_MOUSE_OK x y`. Gate **`smoke-mouse`** injects an
absolute move + click via **QMP `input-send-event`** (the real virtio-input path)
→ `PRADYOS_MOUSE_OK` (abs 16000,12000 → pixel 500,281). **Ring 3 now draws to the
screen, reads the keyboard, AND tracks the pointer.**
**Per-client surfaces + compositing (DDR-706) COMPLETE:** client windows.
`kernel/syscall/sys_surface.c` owns a 16-entry table of PMM-backed BGRA surfaces
(≤512×512); the kernel maps each into **both** the owning client (to draw) and the
compositor (to read) by physical address — the `SYS_FB_MAP` shared-page model, so
compositing is copy-free. Five syscalls (48–52): `SURFACE_CREATE`/`MAP`(owner)/
`COMMIT`/`POLL`(compositor lists committed)/`CMAP`(compositor read-maps). The
compositor polls committed surfaces and blits each at its `(x,y)` onto the desktop
(`PRADYOS_SURFACE_OK <id>`). `user/surfacetest.c` commits a 64×64 green window at
(100,100). Gate **`smoke-surface`** (client commit → compositor composite). **Ring 3
apps can now render windows the compositor composites.**
**Named-agent UI panels (DDR-707) COMPLETE:** the compositor renders the 8 named
agents (KRYOS, PRAX, LUMYN, AHNIS, IRIS, RUFLO, HERMES, SOLIN) as cards with a
status dot tied to AETHER's roster. Assessment found AETHER had no per-name
registry, so one read-only syscall was added: `kernel/syscall/sys_aether.c` gains
an 8-slot active-bit roster (`g_roster`); `SYS_SPAWN_AGENT`'s 3rd arg is now the
roster slot, and **`SYS_AGENT_ROSTER` (53)** copies the bits to ring 3. The daemon
spawns the test agent into slot 0 → **KRYOS active**, the rest inactive. The panel
uses an extended 8×8 font (added K/Y/P/X/H/F glyphs); on a roster change it reports
`AGENT <NAME> active|inactive` ×8 + `PRADYOS_AGENTS_OK`. Gate **`smoke-agents`**.
**Surface z-order + focus + input routing (DDR-708) COMPLETE:** overlapping windows
stack, one holds focus, and keys route to it. Surfaces gain `z` + `focused` + a
per-surface key ring; `SYS_SURFACE_POLL` returns them z-sorted (back-to-front).
Three syscalls (54–56): `SURFACE_RAISE` (top + focus), `SURFACE_SENDKEY`
(compositor forwards a key), `SURFACE_GETKEY` (owner drains). The compositor
composites in z-order (`PRADYOS_ZORDER`), reports focus (`PRADYOS_FOCUS id=`), and
forwards non-shortcut keys to the focused window; `surfacetest` raises window B over
A and receives the routed key (`PRADYOS_FOCUS_KEY`). Input model: the compositor
arbitrates the single global keyboard and forwards to the focused window's private
ring (no client shared-ring race). Gate **`smoke-focus`**.
**Sun-driven OKLab ambiances + animated toggle (DDR-709) COMPLETE:** the brief's
signature UI (§1–§3). The compositor renders the four time-of-day ambiances
(DAWN/DAY/DUSK/NIGHT) with **genuine OKLab** colour interpolation (a libm-free
Newton-`cbrt` OKLab transform — the musl subset has no math lib; sRGB lerps were
rejected per the brief). A new read-only **`SYS_CLOCK` (57)** returns
seconds-since-midnight from the RTC (the vDSO clock is monotonic, not wall-clock),
selecting the ambiance by hour + re-transitioning at boundaries. The mode toggle
now animates the accent in OKLab (`PRADYOS_TOGGLE_ANIM_OK`). A startup demo-cycle
proves all four ambiances (`PRADYOS_AMBIANCE …` + `PRADYOS_AMBIANCE_OK`). Gate
**`smoke-ambiance`**.
**Window decorations + drag-to-move (DDR-710) COMPLETE:** windows have a title bar
and can be dragged. New syscall **`SYS_SURFACE_MOVE` (58)** repositions a surface
(owner or the `CAP_SOVEREIGN` compositor). The compositor draws an 18 px title-bar
decoration (the drag handle) above each window and runs a drag state machine off the
absolute pointer: button-down on the topmost title bar raises+focuses + starts a
drag (`PRADYOS_DRAG_START`), pointer moves reposition the window (`SYS_SURFACE_MOVE`,
recomposited), button-up drops it (`PRADYOS_DRAG id=N x= y=`); a click elsewhere
stays a plain click. Gate **`smoke-drag`** (QMP title-bar drag of window B →
`PRADYOS_DRAG id=1 x=380 y=360`). **PRADYOS now has direct-manipulation windows:
stack, focus, key routing, and drag-to-move.** 42 gates total.
**Window close + resize (DDR-711) COMPLETE:** windows now have a full lifecycle.
Two new syscalls: **`SYS_SURFACE_CLOSE` (59)** frees a surface's PMM buffer + slot
(owner or the `CAP_SOVEREIGN` compositor; the owner path unmaps its VA first so a
stale access faults to a clean user-kill, ADR-021), and **`SYS_SURFACE_RESIZE`
(60)** swaps in a fresh zeroed buffer of the new size keeping position/stack/focus
(owner-only). The compositor now recomposites when the live-surface set **shrinks**
(`ns != composited`), erasing a closed window and printing `PRADYOS_SURFACE_GONE`.
`surfacetest` grows a third window C, resizes it 64×64→96×96 (`PRADYOS_RESIZE_OK`),
then closes it (`PRADYOS_CLOSE_OK`) — A and B stay up so the prior gates are
unaffected. Gate **`smoke-winops`** (client-driven, GPU). 43 gates total.
**Glass panels + particle field (DDR-712) COMPLETE:** the desktop gains the brief's
signature depth (§1/§9) in the software compositor. A per-ambiance **particle
field** (`render_particles`) draws over the background: DAWN 120 motes, DUSK 60
embers, NIGHT 200 stars (twinkle + 4 bright), DAY none (mesh deferred) — a
deterministic LCG-seeded pool alpha-blended via a new `blend_px` (FB-read kept to
the few hundred particle px/frame). The agent cards (DDR-707) are now **frosted
glass** (`glass_card`): a computed translucent bg+white tint (no FB reads) + 1px
accent border, status dots unchanged. The compositor announces
`PRADYOS_PARTICLES_OK` + `PRADYOS_GLASS_OK` on its first render. Gate
**`smoke-visual`** (client-driven, GPU). 44 gates total.
**Agent-card click → AETHER (DDR-713) COMPLETE:** the agent cards are now
interactive — the desktop's first UI→agent action. A new `agent_card_hit()` maps a
pointer button-down to a roster slot (checked before the title-bar drag / plain
click); on a hit the **sovereign** compositor calls `SYS_SPAWN_AGENT(_, name, slot)`
(35) — the kernel spawn hook loads the embedded agent ELF as a `CAP_AGENT` process
and lights `g_roster[slot]`, so the card's dot turns green. It prints
`PRADYOS_AGENT_TRIGGER name=<N> slot=<i> pid=<p>`; no new syscall (authority stays
the kernel flag — the compositor only launches). `mouse_inject.sh` gained optional
`ABSX`/`ABSY` (defaults unchanged, so `smoke-mouse` is untouched). Root-cause fix
folded in (DDR-713 D4): `aether_set_spawn_hook` is now registered **before the
first user process is spawned** — the preemptive scheduler runs user threads
while kmain is still booting, so the sovereign UI could click before the
late-registered hook existed and get `-ENOSYS` (a boot race only the daemon-only
call pattern had masked). Gate **`smoke-agent-click`** (GPU + tablet, QMP) clicks
card 1 (PRAX) → trigger + roster lights PRAX active. 45 gates total.
**APIC stage A (DDR-714) COMPLETE:** the ADR-006 migration begins. New
`kernel/apic/lapic.c`: MADT parse (LAPIC base + CPU count for stage B), the
LAPIC page identity-mapped **uncached** (the ECAM pattern) into the shared boot
PDPT, software-enable via SVR (spurious vector 0xFF) + TPR=0, and the **APIC
timer calibrated against the PIT** (100 ms free-run, div 16) then run periodic at
100 Hz on **new vector 48** — `isr.asm`/IDT grew one stub, and the tick body
(g_ticks/vDSO/sched_tick/lwIP/signals) is a shared `timer_tick` helper used by
both timer paths (EOI-first preserved; the APIC path uses `lapic_eoi`). Once
armed, **PIT IRQ0 is masked** (new `pic_mask`) so exactly one timer drives the
system; device IRQs (kbd/COM1/PCI INTx) stay on the 8259 (hybrid virtual-wire).
No MADT → PIT retained (fallback). Stage B = SMP (INIT-SIPI + per-CPU + a
spinlock ADR superseding ADR-016's masking); stage C = I/O APIC + MSI-X. Gate
**`smoke-apic`**. 46 gates total.
**SMP stage B (DDR-714 / ADR-029) COMPLETE:** real multi-core bring-up, APs
**parked**. `arch/x86_64/ap_boot.asm` is a position-fixed 16-bit trampoline the
BSP copies to physical 0x8000 (SIPI vector 0x08): each AP goes real mode →
PAE+LME → long mode directly on the kernel master CR3, loads a per-AP stack +
entry from the BSP-filled mailbox, and lands in `smp_ap_entry` — announces under
a new **spinlock** (`kernel/include/spinlock.h`, the ADR-029 primitive), marks
itself online (atomic), and parks (`cli/hlt`; no scheduler, no IRQs, so ADR-016's
masking stays valid for the one scheduling CPU). `kernel/apic/smp.c` drives the
SDM MP-init sequence (INIT, 10 ms, SIPI ×2 @200 µs) one AP at a time off the
MADT LAPIC ids (now collected with the enabled-flag check); `lapic_send_ipi`
waits out the ICR busy bit. `boot_test.sh` gained a `QEMU_SMP` knob; every
existing gate stays `-smp 1` (`online=1/1`). Gate **`smoke-smp`** (`-smp 4` →
3 APs → `[smp] cpus online=4/4`). Distributed scheduling = a future ADR.
47 gates total.
**Window titles + close button (DDR-715) COMPLETE:** direct-manipulation
windowing is finished. New **`SYS_SURFACE_SET_TITLE` (61)** (owner-only,
≤15 chars via `copyinstr`); `struct surface_info` carries `title[16]`
(kernel/compositor/surfacetest updated together — in-tree ABI). The compositor
draws the title in the DDR-710 title bar (font gained B/C/T/W glyphs) and a
12×12 red **close box** at the bar's right; the pointer hit-test checks the box
**before** drag-start and `SYS_SURFACE_CLOSE`s the window (`PRADYOS_WM_CLOSE`),
with the DDR-711 shrink detector repainting (`PRADYOS_SURFACE_GONE`).
`surfacetest` titles its windows ALPHA/BETA/GAMMA (`PRADYOS_TITLE_OK`). Gate
**`smoke-wmclose`** (GPU + tablet, QMP clicks GAMMA's close box). `smoke-drag`
unaffected (its click at x=160 is left of B's box at x≥192). 48 gates total.
**Ambiance backdrops (DDR-716) COMPLETE:** the brief's §1 background features.
A new sqrt-free `radial_glow` primitive (quadratic falloff over `blend_px`)
draws, per ambiance: **DAY** 3 gradient-mesh nodes, **DUSK** the sun-bloom at
(85%,90%) warm orange, **NIGHT** two nebulas (`#120024` / `#001220`); DAWN
stays motes-only. Backdrops render only on **settled** frames (`g_settled`,
set on an OKLab transition's final frame) so the 6–8 lerp frames — and the
gates' timing — stay cheap. First settled render per ambiance prints
`PRADYOS_BACKDROP <NAME>`, then `PRADYOS_BACKDROP_OK` after all four (the
startup demo cycle covers them). Mesh/nebula animation + horizon bands stay
deferred. Gate **`smoke-backdrop`** (client-driven, GPU). 49 gates total.
**Window minimize + restore (DDR-717) COMPLETE:** compositor-local (no kernel
change) — a minimized window is skipped when compositing and hit-testing
(`g_min_mask`); its surface stays committed. An amber **min box** sits 2 px left
of the close box (hit-test order: min → close → drag); a hit prints
`PRADYOS_WM_MIN id=` and repaints. The **`r` key** restores all
(`PRADYOS_WM_RESTORE`; per-window restore needs a dock — deferred; `r` no longer
routes to the focused window, documented). Maximize + pointer resize handles
stay deferred pending a surface **event channel** (the client must redraw at a
compositor-chosen size). Gate **`smoke-wmmin`** (GPU + tablet + HMP: click B's
min box, then `r`). 50 gates total.
**Surface event channel + pointer resize (DDR-718) COMPLETE:** the Wayland
`configure` shape in miniature. Each surface gains an 8-entry typed-event ring;
**`SYS_SURFACE_SENDEV` (62)** (compositor/owner push, drop-on-full) and
**`SYS_SURFACE_GETEV` (63)** (owner drains, `-EAGAIN` when empty). Event 1 =
`SURF_EV_RESIZE_REQ(w,h)`. **Authority is unchanged**: the compositor only
requests; the owner performs `SYS_SURFACE_RESIZE`, re-maps, redraws, and
re-commits. First consumer: dragging a window's **bottom-right 14×14 corner**
sends the request on button-up (clamped 32..512; `PRADYOS_RESIZE_REQ`);
`surfacetest`'s loop honors it on B (`PRADYOS_EV_RESIZE_OK`). `drag_inject.sh`
gained `SX/SY/EX/EY` overrides (defaults unchanged → `smoke-drag` untouched).
Maximize (saved-geometry restore on this channel) is the next natural consumer.
Gate **`smoke-evresize`** (GPU + tablet, QMP corner drag). 51 gates total.
**Window maximize + geometry restore (DDR-719) COMPLETE:** the event channel's
second consumer. A green **max box** (third box, `x+w-44`) toggles: maximize
saves `{x,y,w,h}` (per-id arrays + `g_max_mask`), requests **512×512** (the
`SURFACE_DIM_MAX` cap — full-screen waits on a larger surface budget) via
`SURF_EV_RESIZE_REQ`, and moves the window to (8,26); a second click restores
the saved geometry. Prints `PRADYOS_WM_MAX` / `PRADYOS_WM_UNMAX`; the owner
redraws through its DDR-718 handler. The crowded 64-px title bar keeps a 20-px
drag region, so `drag_inject.sh`'s default start moved to pixel (150,130) —
`smoke-drag` re-verified. Gate **`smoke-wmmax`** (two sequential QMP
injections: maximize, then restore at the relocated box keyed on the client's
ack). 52 gates total.
**SMP locking stage 1 (ADR-030) COMPLETE:** the distributed-scheduling
migration begins (staged; each stage CI-green before the next). **PMM** and
**console** swap ADR-016's interrupt masking for per-subsystem spinlocks
(`spin_lock_irqsave` — identical one-CPU semantics, cross-CPU exclusion added);
their local `irq_save/irq_restore` helpers became lock wrappers so no call site
changed. **kheap** — whose slab lists previously had *no* mutual exclusion —
gains a heap lock over every public entry point (kmalloc/kfree, the
pcb/cap/ipc pools, ptnode, outstanding). Lock order: heap → PMM (no cycle).
Each AP now allocates+frees a PMM page and a slab object through the locks
before parking (`[smp] cpu N locks OK`). Stages 2–4 (per-CPU GS state, the
scheduler ring under its lock + AP kernel threads, user threads on APs) are
scoped in ADR-030. Gate **`smoke-smplock`** (`-smp 4`). 53 gates total.
**Per-CPU identity (ADR-030 stage 2, DDR-SMP-2) COMPLETE:**
`kernel/apic/percpu.{c,h}` — a `struct percpu {cpu_idx, apic_id}` array with
`this_cpu()` resolved by **LAPIC-ID lookup**, deliberately NOT `%gs`-based yet:
without SWAPGS discipline a ring-3 `gs` selector reload could clobber a MSR-set
base and let user code break the kernel (ADR-021 isolation) — GS+SWAPGS belongs
to stage 3's syscall-path rework. The BSP records its roster slot after
`lapic_init` (`[percpu] bsp idx= id=`); each AP records + round-trips its entry
before parking (`[smp] cpu N percpu OK`). Fields grow with the stage that uses
them (no dead members). Gate **`smoke-percpu`** (`-smp 4`, forbids
`percpu FAIL`). 54 gates total.
**SWAPGS + GS percpu (ADR-030 stage 3a, DDR-SMP-3a) COMPLETE:** proper SWAPGS
discipline on all four ring-transition sites (the complete set per the gs
audit): `syscall_entry` (unconditional swap at entry + before SYSRET),
`isr_common` (conditional on the frame's CS RPL, mirrored at IRETQ — a thread
that context-switches away mid-ISR resumes with the same frame, so pairing is
per-frame), `enter_user_mode` and `signal_sigreturn` (swap before IRETQ).
Convention: kernel GS base = this CPU's percpu; parked in `KERNEL_GS_BASE`
while ring 3 runs. `struct percpu` gained `self` as member 0;
`percpu_init_cpu` loads `IA32_GS_BASE`; **`this_cpu()` is now one
`mov %gs:0` read** — the stage-2 hazard (user gs-selector reload) is closed
properly, and per-CPU state is reachable from any kernel context (the stage-3b
prerequisite). A one-time probe in `sys_getpid` verifies `%gs:0` from a
ring-3-entered syscall (`[percpu] gs OK (syscall ctx)`). Gate **`smoke-swapgs`**
(`-smp 4`, forbids `gs FAIL`/`percpu FAIL`). 55 gates total.
**Per-CPU scheduler state (ADR-030 stage 3b, DDR-SMP-3b) COMPLETE:**
`current_thread` and the SYSCALL kernel-stack top moved off globals into the
percpu area — `current` @`%gs:8`, `kstack_top` @`%gs:16` (compile-time
asserted). `sched.h` makes `current_thread` a macro over `this_cpu()->current`,
so every read/write site resolves per-CPU unchanged; `syscall_entry.asm`'s
stack switch is now `mov rsp, [gs:16]` (the entry `swapgs` precedes it); the
`syscall_kstack_top` global is gone. Boot order (the DDR-713 lesson):
`percpu_init_early()` claims the BSP slot right after `gdt_init` (whose gs
selector reload zeroes the base) — before anything schedules;
`percpu_init_bsp` later fills the LAPIC id and migrates slots if the BSP's
roster index isn't 0 (collision-proof vs an idx-0 AP); APs init percpu first
thing in `smp_ap_entry`. The `sys_getpid` probe also verifies
`this_cpu()->current` from ring-3 syscall context. Each CPU now tracks its own
running thread — the 3c prerequisite. Gate **`smoke-percpu-sched`** (`-smp 4`).
56 gates total.
**AP work dispatch (ADR-030 stage 3c-alpha, DDR-SMP-3c-alpha) COMPLETE — the
first real multi-core execution.** New **wake IPI on vector 49** (one more ISR
stub; the handler just EOIs — its job is breaking `hlt`). Each AP now idles
with its LAPIC software-enabled (`lapic_ap_enable`) and IF set, draining a
**single-slot percpu mailbox** (`job` @`%gs:24`) per wake; CPL0→CPL0
interrupts use the trampoline stack (no TSS needed) and the 3a conditional
swapgs correctly leaves AP kernel GS in place. BSP API: `smp_run_on(idx, fn)`
(store-release + fixed IPI) / `smp_job_done(idx)` — single-producer/
single-consumer atomics, no lock. At boot the BSP dispatches a proof job to
every AP (`[smp] cpu N job OK` ×3, `[smp] jobs done=3`). Known deferral: the
spurious vector (0xFF) has no IDT gate (pre-existing since stage A; QEMU never
delivers spurious). Full 3c (APs in the scheduler: per-CPU TSS/idle, ring
lock, preemption IPIs) remains. Gate **`smoke-smpjob`** (`-smp 4`).
57 gates total.
**Sched spinlock + cross-CPU wake (ADR-030 3c-locks-1) COMPLETE:** the full-3c
prerequisite campaign begins at the scheduler. `sched.c`'s masking helpers now
acquire **`g_sched_lock`** (`spin_lock_irqsave`, the stage-1 pattern) — held
**across `context_switch`** with the classic handoff (the resuming thread's
`irq_restore` releases; ownerless test-and-set makes that sound), and the one
subtle case handled: a **brand-new thread's first entry** has no resumed frame,
so `thread_trampoline` releases the lock itself (under masking the crafted
RFLAGS auto-released via `popfq`; a lock would have leaked → deadlock).
`sched_unblock` is now an **atomic CAS** (BLOCKED→READY — a pure state
transition, safely callable from an AP; the BSP's locked walk observes READY
within a tick). First cross-CPU scheduling act: a BSP thread blocks, an AP job
wakes it (`[smp] cross-wake waiting` → `[smp] cross-wake OK`). Topology
mutations remain BSP-only until their slices. Gate **`smoke-crosswake`**
(`-smp 4`). 58 gates total.
**Boot authority race FIXED (DDR-boot-authority-race):** the recurring
`smoke-agents` CI failure (3× GitHub-only) root-caused — `elf_load` enqueued
the new thread READY, and kmain set `is_sovereign`/`is_agent` *after* it
returned; under load the daemon ran its `SYS_SET_MODE`/`SYS_SPAWN_AGENT`
self-checks before the flag landed (`-EPERM` = the observed `rc=-1` +
`MODE_TOGGLE_FAIL`). `sched_create_user` now returns the thread **BLOCKED**;
`user_boot_from_sfs` (new `sovereign` param, 13 sites) and the agent spawn
hook grant authority **before** `sched_unblock` — the pre-authority window is
gone by construction (the compositor's identical latent race closed too). The
earlier `smoke-agents` timeout bump treated a symptom; no new gate — the
existing agents/aether/mode gates assert the fix every run.
**Block-path locking (DDR-SMP-3c-locks-2):** `virtio_blk`'s per-disk
serialization is a **sleep-mutex** (`busy` is held across `sched_block()`
while the device DMAs), so making it cross-CPU-safe means an **atomic**
acquire (`__atomic_exchange_n` acquire / `__atomic_store_n` release), NOT a
spinlock — a spinlock held across a block would deadlock spinners. No behavior
change on the single CPU that does block I/O today; the FS gates
(`smoke-fs`/`-rw`/`-sfs-rw`/`-ext4`) + `smoke-user` are the regression surface,
so no new gate. (Completion fields `done`/`waiter` stay under the busy holder +
the owning-CPU INTx; they get their own review when device IRQs move off the
BSP — DDR-714 stage C.) Continues the full-3c prerequisite lock campaign.
**VFS/mount locking (DDR-SMP-3c-locks-3):** the block lock (locks-2) guards the
device DMA, but each VFS op mutates the FS driver's *in-memory* metadata (SFS
journal/B-tree, FAT cursor) *around* the block calls — unprotected. A per-mount
**sleep-mutex** (`vfs_mount.busy`, atomic acquire/`yield` + release) now wraps
the 9 data-path entry points (`open`/`create`/`read`/`write`/`unlink`/`readdir`/
`txn_*`); sleep-mutex not spinlock (the op blocks on I/O), lock order always
mount->blk. The mount TABLE stays BSP-only (topology, like locks-1); this slice
is corruption-safety, NOT transaction isolation (multi-syscall txn interleave
is already possible single-CPU — unchanged). No new gate; the FS/user gates
assert every locked path.
**IPC + broadcast-bus locking (DDR-SMP-3c-locks-4):** the synchronous IPC
endpoint and the sovereign broadcast bus closed their lost-wakeup race with
`cli/sti` — which masks only the LOCAL CPU, so across CPUs a sender/publisher
could miss a waiter that had not yet published its BLOCKED state. Fixed with
per-object `spinlock_t` (endpoint; bus-list + per-subscriber, the bcast queue
being MPSC) and a new scheduler primitive **`sched_block_on(lk)`** that sets the
caller BLOCKED *under* `lk` before releasing + switching away, so a waker
serialized after the release always sees BLOCKED and its `sched_unblock` CAS
can't be lost. Lock order bus->subscriber; IRQs stay masked across the switch
(RFLAGS preserved), matching the old path. The async SPSC `ipc_ring` is already
cross-CPU-correct (acquire/release on head/tail under a strict 1-producer/
1-consumer contract) — unchanged. No new gate; the agents/aether/mode gates
drive the endpoint + bus every run. Completes the subsystem-lock phase of the
full-3c campaign (scheduler→block→VFS→IPC).
**Capstone (ADR-031): APs enter the scheduler — staged.** The subsystem-lock
phase being complete, ADR-031 governs the payoff: APs stop parking and run
threads. It supersedes DDR-SMP-3c-locks-1's BSP-only-topology restriction under
a defined locking discipline, in four sub-slices (cap-1..4), each CI-green
before the next.
**cap-1 — per-CPU TSS/GDT/TR (DDR-SMP-3c-cap-1):** the single global TSS +
`0x28` descriptor + `LTR` become per-CPU (TR is per-logical-CPU; each consults
its own RSP0, and two CPUs can't `LTR` one descriptor — busy bit → #GP). The
GDT now holds `PERCPU_MAX` TSS descriptors (CPU i = `0x28+i*0x10`); `tss[]` is
indexed by `cpu_idx`; `tss_set_rsp0` targets the running CPU's own TSS. APs load
the shared `gdt64` (via `gdt_init`) BEFORE setting their GS base, then `LTR`
their own TSS — proven at boot by `[smp] cpu N tss OK` (asserted by `smoke-smp`,
`tss FAIL` forbidden). The BSP-migration case re-homes TR onto its final
`cpu_idx`. Contract-neutral (RSP0 unused until ring-3 runs on an AP, cap-4); no
new gate, no behavior change. Next: cap-2 (per-CPU idle + locked topology).
**cap-2a — SMP-safe scheduler internals (DDR-SMP-3c-cap-2a):** cap-2 split so
the hot-path rewrite lands separately from the AP-scheduling flip. The scheduler
is one shared ring walked from each CPU's (`%gs`) `current`; `runnable()`
treated `THREAD_RUNNING` as pickable, so a second CPU walking the ring could
grab a thread live on another CPU. Fixed with an `on_cpu` claim (tcb field, −1 =
free): `schedule()` picks only `READY && on_cpu<0`, claims/releases under
`g_sched_lock` (atomic — no double-run). `on_cpu` clears whenever we switch AWAY
from `prev` (not only when RUNNING) — a thread that blocked must release its CPU
or it fails the `on_cpu<0` test after unblock and never runs again (caught
locally on the first block-I/O). Topology (`sched_create` insert, `sched_exit`
reparent+ZOMBIE, `sched_destroy` unlink) now under `g_sched_lock`
(DDR-locks-1's BSP-only restriction superseded); the reaper unlinks under the
lock and frees outside it. User creation (`sched_create_user`/`_clone`) inserts
BLOCKED atomically and the caller unblocks after full init (the create-then-init
race — clone keeps its `cli` for the GLOBAL `syscall_user_*` snapshot, a cap-4
per-CPU item). BSP-only behavioral **no-op** — all 58 gates green prove the
internals before cap-2b adds real AP concurrency. No new gate.
**cap-2b — APs enter the scheduler (DDR-SMP-3c-cap-2b):** the APs leave their
mailbox park loop and run **kernel** threads from the shared ready ring — the
first time a ring thread executes off the BSP. Per-CPU idle (`is_idle`; BSP
static, AP idles `kmalloc`'d — a full TCB × 16 in BSS overruns the low-mem cap);
a CPU picks only its OWN idle (but must, since the idle is its main context —
the BSP idle runs `sched_demo`). `sched_ap_enter` waits on `g_sched_ready`
(APs come online before `sched_init`) then joins, draining the mailbox so
`smp_run_on`/`smoke-smpjob`/`smoke-crosswake` keep working.
`thread_trampoline` now `schedule()`s on a kernel thread's return instead of
`hlt` (an un-preempted AP would wedge in a finished thread). USER threads stay
**BSP-pinned** (`pickable`'s `is_bsp` guard + a `struct percpu.is_bsp` byte) —
ring-3 on an AP needs per-CPU SYSCALL state (cap-4). New gate `smoke-smpsched`
(`-smp 4`): a probe kernel thread runs on a non-BSP CPU → `[smp] sched
cross-CPU OK`. **59 gates.** Three root-caused SMP bugs en route (BSS overflow;
AP-before-`sched_init` ordering; own-idle pickability).
**cap-3 — per-AP LAPIC-timer preemption (DDR-SMP-3c-cap-3):** each AP arms its
own LAPIC timer at the BSP-calibrated 100 Hz count (`lapic_timer_ap_arm`; same
bus clock — no recalibration) in `smp_ap_entry`, so its vector-48 tick drives
`sched_tick`→`schedule()` — real preemption on every CPU (cap-2b was
cooperative-only on APs). **Global tick side-effects stay BSP-only:**
`timer_tick` (g_ticks, vDSO wall clock, lwIP timers, ring-3 signal delivery)
runs only on the BSP; an AP's tick calls just `sched_tick` — otherwise 4 CPUs
inflate `g_ticks` 4×, shrinking every tick-based deadline (first symptom:
`smoke-crosswake` flaked). Per-CPU `percpu.ticks` counts each CPU's timer
firings; gate `smoke-smppreempt` asserts a non-BSP CPU's counter advances →
`[smp] ap preempt OK`. **60 gates.** Second root-cause en route: the
crosswake/preempt proofs picked their target AP as "not the current CPU" — but
proof threads themselves now migrate (cap-2b), so on an AP that resolved to the
BSP, whose mailbox nothing drains (2/8 flaky); both proofs now select by
`is_bsp` (10/10).
**cap-4 — user threads on APs (DDR-SMP-3c-cap-4): ADR-031 COMPLETE.** The BSP
pin on ring-3 threads is gone — every CPU schedules, preempts, and runs user
processes. (1) The SYSCALL-entry register snapshot (`syscall_user_*` globals —
two CPUs would clobber each other's fork state) moved into `struct percpu` at
fixed gs-relative offsets (`u_rsp..u_rflags` @56..120, static-asserted);
`syscall_entry.asm` writes `[gs:...]`, the fork paths read `this_cpu()->u_*`.
(2) AP timer returns to ring 3 now deliver pending signals (`signal_deliver` in
the AP branch; global tick stays BSP-only). (3) `pickable()` drops the
`is_user` rejection. (4) **The AP trampoline's machine state is not the
BSP's** (found by the gate: every user process on an AP died — `#UD` at its
first `syscall`, RSVD `#PF` on NX pages): APs now arm per-CPU `cpu_enable_sse`
(musl XMM), `EFER.NXE` (W^X), and the SYSCALL MSRs (`syscall_init_ap`) in
`smp_ap_entry`. Proof: `schedule()` flags when an AP claims a user thread; gate
`smoke-smpuser` asserts `[smp] user on AP OK` plus the user programs' own
sentinels (they must run *correctly* on APs). **61 gates.** This completes the
ADR-030 staged migration (ADR-016 fully superseded for the scheduler) — begun
at stage-1 subsystem locks, ended with ring 3 on every core.
**DDR-714 stage C1 — MSI-X for virtio-blk:** stage C (IRQ routing off the
8259/INTx) starts with MSI-X, NOT the I/O APIC — on q35 the PIC-mode
`Interrupt Line` values the drivers read don't match the PCI-INTx→GSI mapping,
and MSI-X sidesteps the question: the device writes its interrupt message
straight to the LAPIC. `virtio_pci_msix_setup` (cap 0x11 walk, table entry 0 =
`0xFEE00000|apic_id<<12` + vector, queue 0 → entry 0 with 0xFFFF-readback
verification, INTx-disable) gives each of the 4 disks its own vector 50..53
(IDT 50→54 stubs; `msix_register` + LAPIC-only-EOI dispatch in idt.c) with a
clean INTx fallback. Unshares the blk lines (blk0/1+net on 11, blk2/3 on 10 —
the chained-INTx workaround no longer covers disks) and is the prerequisite for
multi-in-flight I/O. Destination = BSP for now (locks-2's completion-field
review comes with distribution, C2/C3). `smoke-fs` now asserts `msix vec=50`
(no silent fallback). No new gate; 61 total.
**Exit-vs-collect kstack use-after-free FIXED (DDR-SMP-exit-stack-race,
CI-caught):** `sched_exit` set ZOMBIE, RELEASED `g_sched_lock`, woke the
waiter, then switched away — post-cap-4 a parent's `wait4` on another CPU could
collect + `sched_destroy` (free the kernel stack) before the dying thread's
`context_switch` left that stack. KASAN poison made it legible (kernel #GP,
`RAX=0xDEADBEEF...`, TCG-only). Fix by construction: `schedule()` split into
`schedule_locked(fl)`; `sched_exit` holds the lock ACROSS its final switch (the
locks-1 handoff releases it after the switch), so any collector's
`sched_destroy` serializes behind the release — the stack is provably free of
its owner when freed. No new gate (`smoke-syswait`/`smoke-user` ride the path).
**DDR-714 stage C2 — MSI-X for virtio-net + virtio-input:** the remaining
INTx consumers move to per-device vectors — net on 54 (BOTH queues, RX 0 + TX
1, routed to table entry 0 via `virtio_pci_msix_setup`'s new `nqueues` param),
the tablet on 55 (event queue). IDT 54→56 stubs; the drivers keep the C1
split (shared `*_complete` body; the INTx fallback keeps its ack-gate).
**No virtio device touches the 8259 anymore** — the only PIC lines left are ISA
(keyboard IRQ1, COM1 RX IRQ4). `smoke-net-lo` asserts `msix vec=54`; the
pointer gates prove vec 55 functionally (QMP events arrive only via it).
Destination still the BSP; distribution (+ completion-field review) is C3.
No new gate; 61 total.
**DDR-714 stage C3 — blk vectors distributed across APs (stage C COMPLETE):**
disk completions now run OFF the BSP — blk unit i's vector targets roster CPU
`1+(i%(n-1))`. The locks-2 D2 completion review, done: the old `cli` around
`done`/`waiter` masked only the local CPU, so a completion IRQ on another CPU
could fire between the requester's `done` check and its BLOCKED transition —
its wake CAS a no-op → sleep forever. Fixed with the locks-4 pattern: a
per-device `compl_lock` guards `done`/`waiter` (+the short publish/notify), the
IRQ handler takes it (irqsave), and the requester waits via
`sched_block_on(&compl_lock)` — BLOCKED published under the lock, the wake
can't miss. The virtq needs no extra lock while the one-in-flight `busy`
sleep-mutex holds (submit/complete never overlap in time; ordering via
`virtio_mb` + the lock). **net + input stay BSP-routed by decision** (net's
completion calls into lwIP, which is single-threaded by design; input folds
into BSP-polled globals — documented in the DDR). Gate `smoke-msixap`
(`-smp 4`): `[blk] msix on AP OK` + FS sentinels (correct I/O under cross-CPU
completion). **62 gates.**
**Multi-in-flight block I/O (DDR-BLK-1):** the payoff C1–C3 unblocked. The
one-in-flight `busy` sleep-mutex is DELETED: each disk gets `VBLK_NREQ` (8)
per-request slots (32-byte header+status strides in the existing reqbuf page;
per-slot `done`/`waiter`; `head2slot[]` maps used-ring heads back). ALL vq +
slot state moves under `compl_lock` (submit-side add/publish and the vector
CPU's pop now genuinely interleave — C3's "no vq lock" rested on one-in-flight);
slot exhaustion sleeps via `sched_block_on` (never spins), woken on slot free.
A caller blocks only on ITS OWN request — others proceed concurrently on any
CPU. Proof: two kernel threads keep interleaved reads in flight on one disk,
each round-tripping cleanly → `[blk] multi-inflight OK`; gate `smoke-blkmq`
(`-smp 4`) + the FS family re-verifies correctness. **63 gates.**
**Per-CPU runqueues + work stealing (DDR-SMP-rq-1):** the scheduler hot path
no longer scans the shared ring — each CPU picks O(1) from its own FIFO
(`g_rq[]`, leaf locks), steals (trylock, own-rq-first release — no
hold-and-wait) when empty, and re-queues a preempted prev at its tail. The
ring survives as the TOPOLOGY list only (reparent/pid_alive/reaper, still
under `g_sched_lock`, which also still covers the switch — the handoff keeps
the resume-before-save argument intact; per-CPU switch locks are rq-2).
`sched_unblock` enqueues on the waker's CPU. Three bugs the gates caught:
idle starvation on empty queues (idles are contexts — rotate through them);
transient READY-but-`on_cpu>=0` entries must be RE-APPENDED, never dropped
(a dropped one is a lost thread — deterministic winops hang); and the surface
client outracing compositor init (COMPOSIT now spawns before SURFTEST + wider
close delay). Gate `smoke-rqstress` (24-thread storm, 3 waves). **64 gates.**
**Window cycling (DDR-720):** Tab is a compositor hotkey (not forwarded to the
focus): each press raises the bottom-most visible, non-minimized surface
(`SYS_SURFACE_RAISE`, sovereign override) — repeated Tab rotates the whole set
in z-order, printing `PRADYOS_WM_CYCLE id=N`. Realized as plain Tab (the PS/2
keymap delivers ASCII; Alt-modifier plumbing stays deferred with the scancode
work). Gate `smoke-alttab` (GPU + sendkey `tab`): ≥2 cycles over ≥2 DIFFERENT
windows — measured 3 cycles over 3 windows. **65 gates.**
**Double-buffered page flip (DDR-721):** `SYS_FB_FLUSH` no longer transfers
into the resource being scanned out (the host could present a half-transferred
frame). The driver creates a SECOND host resource attached to the SAME guest
pages (zero client API change); each flush transfers into the off-screen one →
SET_SCANOUT flips to it → RESOURCE_FLUSH — the displayed resource is always a
complete frame. Single-buffer fallback if the second create fails. Sentinel
`[gpu] page-flip OK` after both resources have presented; gate `smoke-flip`.
**66 gates.**
**Real glass blur + saturation (DDR-722):** glass cards now BLUR the composed
scene beneath them — separable in-place 9-tap box blur (radius 4), card-sized
regions only — with a ×1.3 chroma boost around luma (the brief's frosted pair),
then the tint BLENDS over it (`blend_px` rgba(255,255,255,0.10); the old
opaque precomputed fill would have erased the blur) + the 1px accent border.
Sentinel `PRADYOS_GLASS_BLUR_OK` on the first blurred card; gate
`smoke-glassblur`. Closes the longest-standing deferred visual (since
DDR-712). **67 gates.**
**Multi-stop gradient backdrops (DDR-723):** the base fill under every
ambiance was a flat `g_bg` rect; it is now a 3-stop vertical gradient DERIVED
from the ambiance bg (0.0→bg, 0.35→bg×1.25 horizon lightening, 1.0→bg×0.55
floor darkening, clamped, per-row fills) — so the OKLab ambiance transitions
keep working unmodified and the DDR-716 glows draw over it. Sentinel
`PRADYOS_GRADIENT_OK`; gate `smoke-gradient`. **68 gates.**
**Window decorations (DDR-724):** windows now carry a 1px frame — ACCENT
colored when focused, neutral gray otherwise (a glanceable focus cue) — plus a
fading right/bottom drop shadow (3 `blend_px` strips, α 0.22→0.10). Off-screen
edges clip through `put_px`. The title-bar click boxes (close/min/max) are
unmoved — all pointer gates re-verified. Sentinel `PRADYOS_DECOR_OK`; gate
`smoke-decor`. **69 gates.**
**Scroll-wheel plumbing (DDR-725):** wheel detents flow end-to-end — virtio
`EV_REL/REL_WHEEL` accumulates per-device; `SYS_MOUSE_POLL`'s `mouse_state`
gains a `wheel` field (read-and-clear; all in-tree callers rebuilt); the
compositor routes a type-2 surface event (delta in arg1) to the FOCUSED
window; surfacetest acks `PRADYOS_EV_SCROLL_OK`. New QMP injector
`wheel_inject.sh` (input-send-event wheel-up/down); gate `smoke-scroll`.
**70 gates.**
**Auto ambiance cadence + pre-transition (DDR-726):** the compositor now
advances the ambiance 0→1→2→3→0 automatically through the existing OKLab
transition path, with a gentle accent pulse (`PRADYOS_PRETRANSITION`) in the
final 10% of each period. Time source is the yield-paced frame loop (iteration
count approximating the brief's 900 s; a proper user clock refines this later
— documented in the DDR). Hotkey `k` shrinks the cadence so gate
`smoke-cadence` proves a FULL automatic cycle (pulse + 4 advances →
`PRADYOS_CADENCE_OK`) in seconds. **71 gates.**
**Deferred (DDR-702..709):** the Inter typeface; the
15-min-before pre-transition + 900 s auto cadence; the toggle's spring/ripple
motion; double-buffer / page-flip; relative-mouse + scroll; window move/drag;
alt-tab; window decorations; surface destroy; per-agent live metrics; SFS
`/etc/aether/config`; `CAP_NET` gate; the wlroots/Wayland protocol (out-of-tree
library ports — the standing wall). Min/max buttons + pointer resize **handles**
are the DDR-715 follow-ons (titles + close button shipped).
**Next: ADR-030 full 3c (APs in the scheduler: per-CPU TSS/idle, ring lock,
preemption IPIs — the AP job mailbox is the interim multi-core compute path);
or I/O APIC + MSI-X (DDR-714 stage C); or more visual richness (real glass
blur / gradients). wlroots/Wayland remain out-of-tree.**
**Last updated:** 2026-07-02

## Phase 0 — Toolchain & Build System

Verified on WSL2 Ubuntu-22.04, 2026-06-17: clang 14.0.0, LLD 14.0.0,
NASM 2.15.05, QEMU 6.2.0, rustc/cargo 1.98.0-nightly (target
`x86_64-unknown-none`). `make toolchain-check` and `make smoke` both exit 0.

| Item | Status | Notes |
|------|--------|-------|
| Repo skeleton (mandated tree) | 🟢 COMPLETE | dirs + `.gitkeep`, ADRs, docs |
| Toolchain bootstrap script | 🟢 COMPLETE | `tools/build/setup_toolchain.sh` (WSL2), run OK |
| LLVM/Clang + lld + NASM + Rust nightly | 🟢 COMPLETE | installed + `make toolchain-check` links clean (exit 0) |
| x86_64 triple pinned | 🟢 COMPLETE | `x86_64-elf` confirmed on clang 14 (see ADR-001) |
| QEMU runner harness | 🟢 COMPLETE | `tools/qemu_runner/boot_test.sh` SKIPs cleanly (inert until P1) |
| CI Pipeline (QEMU boot) | 🟢 LIVE | `.github/workflows/ci.yml` runs on push to `main`/`dev/**` + PRs: toolchain-check, `make image` (-Werror), `make smoke` (q35 kernel-sentinel gate), `make smoke-fs` (q35 FAT32 read end-to-end; installs dosfstools+mtools). `smoke` deliberately does not depend on the FAT image so the kernel gate never fails on a missing host FS tool. |

## Component Tracker (Phases 1–8)

| Component | Status | Phase | Notes |
|-----------|--------|-------|-------|
| PRADYOS-BOOT Stage 1 (MBR) | 🟢 COMPLETE | 1 | 512-byte MBR; INT 13h/AH=42h LBA read loads Stage 2, jumps to it. Kernel-ELF load deferred to Phase 2a (no kernel yet). |
| PRADYOS-BOOT Stage 2 | 🟢 COMPLETE | 1 | A20 (fast), INT 15h E820 walk, CPUID vendor + long-mode bit, flat GDT, 32-bit protected-mode switch, `PRADYOS BOOT OK` via COM1 + VGA. `make smoke` PASS. |
| UEFI Boot Path | 🔴 NOT BUILT | 1 | EDK2/OVMF-compatible; deferred (MBR path chosen first per user) |
| Hardware-info handoff struct | 🟢 COMPLETE | 1/2a | Completed in 2a — `kernel/boot_info.h` ABI; Stage 2 fills it at phys 0x4000 (E820 + vendor + LM) and passes the pointer in RDI (see "Boot→kernel handoff struct" below). |
| NEXUS Kernel Entry (asm) | 🟢 COMPLETE | 2a | 64-bit entry + C `kmain` in ring 0; kernel-owned flat GDT loaded (`arch/x86_64/cpu.asm`), CS reloaded via far return. |
| Boot→kernel handoff struct | 🟢 COMPLETE | 2a | `kernel/boot_info.h` ABI; Stage 2 fills it at phys 0x4000 (E820 map + vendor + LM), passes ptr in RDI; kernel re-prints the map. Closes Phase 1's last blocking item. |
| NEXUS Interrupt Handlers | 🟡 IN PROGRESS | 2a/5a | IDT, 48 vectors (32 exceptions + 16 IRQ). Panic = fault + register dump + CR2 + backtrace + clean halt; `int3` recovers. **5a:** a fault from CPL 3 (ring-3 user code) is converted to a clean process kill (`sched_exit`) with a `[trap]` diagnostic instead of a kernel panic — the W^X / guard-page enforcement path (ADR-021). Legacy 8259 PIC remapped + 8254 PIT @100Hz + keyboard IRQ; `sti` on; timer tick verified. APIC deferred to 2b (ADR-006). |
| NEXUS Context Switch (asm) | 🟢 COMPLETE | 2c | `arch/x86_64/context.asm`; measured ~275 cycles / ~107 ns @2.56 GHz (target ≤ 1.5 µs, board). TSC calibrated vs PIT. |
| Physical Frame Oracle / PMM | 🟢 COMPLETE | 2b | Buddy allocator (`kernel/pmm.{c,h}`, ADR-003) seeded from E820; orders 0..10, split/coalesce. Self-test: 0x6FE0 free frames, aligned alloc, balanced release. Manages [16 MiB, 1 GiB). |
| Virtual Memory Manager | 🟢 COMPLETE | 2b/5a | Higher-half kernel @0xFFFFFFFF80000000 (ADR-007); bootloader maps high + low identity; kernel-owned `vmm_map`/`vmm_unmap` (`kernel/mm/vmm.{c,h}`) allocate/reclaim tables from the PMM. Verified leak-free. **5a (ADR-021):** per-process address spaces (`vmm_new_address_space` shares kernel PML4 entries, user range = slot 1; `vmm_map_in` targets a non-active AS; `vmm_destroy_address_space` frees the private subtree); **EFER.NXE enabled, CPUID-gated** so `VMM_NX` is honored. physmap deferred. |
| SLAB Allocator / kernel heap | 🟢 COMPLETE | 2b | `kernel/kheap.{c,h}` on the buddy PMM: size-class slab caches + whole-page large allocs; dedicated PCB/cap/IPC/page-table pools; debug poison + double-free + leak accounting. Stress test: no leak. Build is `-Werror` (C + NASM). |
| Process Control Blocks | 🟢 COMPLETE | 2c | Minimal TCB (`kernel/sched.h`): rsp, kstack, tid, state, quantum. Full PCB (caps, VAS, quotas) later. |
| NEXUS Adaptive Scheduler | 🟡 IN PROGRESS | 2c/5a | Round-robin ready ring, PIT-preemptive, quantum 2 ticks (ADR-008). Two threads verified interleaving. **5a:** per-process CR3 in the TCB; `schedule()` reloads CR3 when switching address spaces (kernel stacks identity-mapped in every AS, so the switch precedes the stack switch safely). 3-lane NAS + AI-hint lane deferred. |
| NCS Capability System | 🟢 COMPLETE | 2c | `kernel/cap.{c,h}` (ADR-009): opaque table-indexed handles, per-process tables on `tcb->caps`, O(1) generation-counter revoke, subset-only restrict/delegate, rights bitmap, guard-before-op. 11/11 tests pass. (MAC token = future external format only.) |
| NIA IPC (Sync + Async) | 🟢 COMPLETE | 2c | All 3 primitives: sync endpoint (block/wakeup), async lock-free SPSC ring, broadcast bus — all capability-gated + resource-bound (`kernel/ipc/`, ADR-010/011). Verified: sync exchange; ring 200 in-order; pub-sub filtered. |
| Sovereign Broadcast Bus | 🟢 COMPLETE | 2c | `kernel/ipc/bcast.{c,h}` (ADR-011): event-mask pub-sub, publish gated by CAP_BROADCAST, subscribe by CAP_IPC_RECV. Filtering verified. |
| Thread block/wakeup | 🟢 COMPLETE | 2c | `sched_block`/`sched_unblock`; `schedule()` skips non-runnable; idle always runnable |
| Syscall Table (200+ calls) | 🟡 IN PROGRESS | 2e | SYSCALL/SYSRET armed (EFER.SCE/STAR/LSTAR/SFMASK), dispatch table + register, capability-aware (`kernel/syscall/`, ADR-012). 4 calls (putc/getpid/yield/exit) exercised from ring 3; more arrive with userspace. |
| Ring-3 user mode (TSS + user segs) | 🟢 COMPLETE | 2e | `kernel/proc/tss.c`, user GDT segs, `sched_create_user`, IRETQ→ring3, cap-gated syscalls. Verified user thread runs + exits cleanly. |
| PRADYOS Extended Syscalls | 🟢 COMPLETE | 6/7 | AETHER NSI 29–38 (mode/queue/audit/mem) + socket 39–42 (ADR-027) + framebuffer 43–45 (DDR-702) + input 46 (DDR-703); all copyin/copyout, capability-gated where applicable. |
| ACPI table parser (RSDP/RSDT/XSDT) | 🟢 COMPLETE | 3 | `kernel/acpi/` (ADR-013): find RSDP, walk RSDT/XSDT, `acpi_find_table`. Unblocks MCFG/MADT/FADT. |
| PCIe Enumeration | 🟢 COMPLETE | 3 | `kernel/drivers/pcie/` (ADR-013): MCFG→ECAM, uncached map, bus-0 scan + device registry. q35: 7 devices incl. virtio-blk/net + VGA. |
| virtio transport (reusable) | 🟢 COMPLETE | 3 | `kernel/drivers/virtio/` (ADR-014): modern 1.0 — PCI caps, BAR/MMIO, status machine, feature negotiation, split virtqueues, notify, ISR. Shared by all virtio devices. |
| Block layer (generic) | 🟢 COMPLETE | 3 | `kernel/drivers/blk/blk.{c,h}`: device registry + read/write dispatch. |
| virtio-blk driver | 🟢 COMPLETE | 3 | `kernel/drivers/blk/virtio_blk.c` (ADR-014/015): interrupt-driven (INTx) read/write. **Multi-instance** (per-disk transport/queue/BAR window) + **per-device serialization** (busy + yield-wait). Verified: sector-0 MBR read, write/read round-trip, 2 disks concurrently. |
| NVMe Driver | 🔴 NOT BUILT | 3 | priority storage (registers with blk layer) |
| GPU Framebuffer | 🟢 COMPLETE | 7-s0 | `kernel/drivers/gpu/virtio_gpu.c` (ADR-028): VirtIO-GPU 2D bring-up (display-info → create_2d → attach_backing → set_scanout → transfer → flush), linear BGRA framebuffer from the PMM pool. Gate `smoke-gpu`. Ring-3 surface via `SYS_FB_*` (DDR-702, gate `smoke-fb`). Double-buffer/page-flip deferred. |
| Network Driver (virtio-net) | 🟢 COMPLETE | 3/NET-A | `kernel/drivers/net/virtio_net.c` (ADR-014): modern virtio-pci, RX/TX virtqueues, MAC, shared-INTx handler; RX delivery fixed in NET-B. Carries lwIP (ADR-025). |
| ACPI Power Management (FADT/MADT) | 🔴 NOT BUILT | 3 | parser ready; MADT→APIC, FADT→power |
| VFS Layer | 🟢 COMPLETE | 4 | `kernel/fs/vfs/` (ADR-015): driver registry + **mount table** (per-mount context vtable; FAT32/SFS/ext4 mountable side-by-side) + `open`/`create`/`read`/`write`/`unlink`/`readdir`, all capability-gated (CAP_FS_READ/WRITE via NCS) + per-thread write budget. Full mount-point namespace deferred. |
| FAT32 (read-write) | 🟢 COMPLETE | 4 | `kernel/fs/fat32/` (ADR-015): BPB parse, FAT chain, 8.3 + **VFAT long-name read** (ADR-020), nested paths. Read-write (4c): create/write/unlink, all-or-nothing alloc, read-back verify (`smoke-fs-rw`). **Timestamps** from RTC (4j). LFN *write* deferred (creates 8.3). |
| RTC / CMOS clock | 🟢 COMPLETE | 3 | `kernel/drivers/rtc/` (ADR-020): wall-clock via ports 0x70/0x71 (BCD/binary, 12/24h, stable read). `rtc_now` + `rtc_fat_datetime`; powers FS timestamps and later CLOCK_REALTIME. (Deferred Layer-3 item, pulled in at 4j.) |
| SOVEREIGN FS (SFS) | 🟢 COMPLETE | 4 | `kernel/fs/sfs/` (ADR-017/018): inode-based CoW B+tree, 4 KiB blocks. **4d:** format/mount/empty-root. **4e:** CoW B+tree create/lookup/open/readdir (split-on-insert; 10-file test). **4f:** file extents (write append/grow + read). **4g:** journal + atomic transactions (commit-record + mount replay). **4h:** snapshots — retained CoW roots; `sfs_open_version` reads a file as-of a snapshot. **4i:** inline LZ4 (`kernel/fs/sfs/lz4.c`, bounds-checked) — **per-extent** compression so compressed files still append; + ~4 KiB inode metadata tags (`sfs_set_tag`/`get_tag`). Verified: 128 KiB compressible → <32 blocks, byte-exact readback, tag survives remount. Next: ext4 read + FAT32 LFN (4j) → Layer 4 gate. Free-space B+tree / snapshot GC deferred. `CAP_FS_SFS_*` reserved. |
| SOVEREIGN FS (SFS) — duplicate row | 🟢 COMPLETE | 4 | (stale duplicate of the SFS row above; see it for detail) |
| ext4 Compatibility | 🟢 COMPLETE | 4 | `kernel/fs/ext4/` (ADR-019, slice 4j): **read-only** (the Layer-4 scope; write is out of scope) — superblock, group descriptors, extent-mapped inodes (depth-0), linear dir scan, nested paths. Verified reading a host `mkfs.ext4 -d` volume (4th disk). Write, multi-level extents, block-mapped inodes deferred. |
| ELF64 loader + W^X (static) | 🟢 COMPLETE | 5a | `kernel/exec/elf.c` (ADR-021): validates ET_EXEC/x86-64, maps each PT_LOAD into a fresh per-process AS with p_flags→W^X perms (text RX, rodata R-NX, data RW-NX; **W+X rejected**), zero-fills BSS, 8 MiB RW-NX user stack + unmapped guard page, SysV `argc/argv/envp/auxv` frame; spawns a ring-3 thread (cap delivered in RDI). Bootstrapped via SFS: the embedded test ELF (`user/hello.asm`) is written to SFS then **loaded back from SFS** — prints `HELLO FROM RING-3`, exits via sys_exit. W^X negative regression (`user/wxviol.asm`: write to RX text → #PF err=0x7 → clean kill, kernel survives). Gate `smoke-user` PASS. COW fork / dynamic linking / AS-reaping deferred. |
| pradyos-init (PID 1) | 🟢 COMPLETE | 5d | `user/init.c` (musl C, not Rust): PID 1, forks+reaps a child, then the system reaper loop; spawns PRISM. Gate `smoke-init`. |
| PRISM Shell | 🟢 COMPLETE | 5e | `user/prism.c` (musl C): serial-console shell, builtins help/echo/cat/run/ls/ps/`mode`/exit; console RX via IRQ4 ring. Gate `smoke-shell`. Agent DSL / job control deferred (ADR-024). |
| musl libc port | 🟢 COMPLETE | 5c/PROC-D | `third_party/musl` subset (libc.a + crt1.o) via `tools/build_musl.sh`, overrides in `third_party/musl-overlay/`; TLS + stdio + printf via SYS_WRITEV. Gate `smoke` (cmusl). |
| prad package manager | 🔴 NOT BUILT | 5 | |
| AETHER Daemon | 🟢 COMPLETE | 6 | `user/aether_daemon.c` (PID-2, CAP_SOVEREIGN): spawns the test agent via SYS_SPAWN_AGENT, reaps children, runs the mode-binding self-check. Gate `smoke-aether`. |
| Ollama IPC Bridge | 🟢 COMPLETE | 6/7 | Ring-3 proxy-socket NSI (ADR-027) + `user/agent_base.c` live mode: HTTP/1.1 `POST /api/generate` over the in-kernel lwIP stack + a hand-written JSON parser. Dev gate `smoke-agent-live` (needs a real Ollama); CI is test-mode. |
| Cloud API Adapters | 🔴 NOT BUILT | 6 | Anthropic/OpenAI/Gemini (the proxy-socket NSI makes these straightforward; not yet written) |
| Agent Capability Enforcer | 🟢 COMPLETE | 6 | `kernel/aether/` + `cap.h` CAP_AGENT/CAP_SOVEREIGN (kernel-set, no self-escalation) + 128 MiB mem cap (OOM kill) + 60 syscall/s rate limit. Gate `smoke-aether-sec`. |
| SOVEREIGN Gate Logic | 🟢 COMPLETE | 6/7 | Global `g_sovereign_mode` (default sovereign auto-approve; manual holds PENDING); `SYS_GET_MODE`/`SYS_SET_MODE` (CAP_SOVEREIGN). Bound to the toggle (DDR-701). Gate `smoke-mode`. |
| Approval Queue System | 🟢 COMPLETE | 6 | `kernel/aether/aether_queue.c`: 256-entry action queue + 4096-entry append-only audit ring (PMM-pool), 60 s TTL, `-EAGAIN` on overflow. Gates `smoke-aether-queue/-sec`. UI panel = compositor slice. |
| Named Agents (KRYOS…SOLIN) | 🟡 IN PROGRESS | 6f/7 | The 8 named agents render as compositor panel cards with active/inactive state tied to AETHER's 8-slot roster (DDR-707, `SYS_AGENT_ROSTER`); the daemon lights KRYOS. Gate `smoke-agents`. Distinct per-persona agent behaviours are still future. |
| Wayland Compositor | 🔴 NOT BUILT | 7 | wlroots/Wayland is a large out-of-tree port (libdrm/EGL/pixman) — standing wall. An **in-house** full-screen compositor over `SYS_FB_*`+`SYS_INPUT_POLL` is the in-progress path (DDR-704), not wlroots. |
| SOVEREIGN MODE UI | 🟡 IN PROGRESS | 7 | Basic in-house compositor renders it (DDR-704, `user/compositor.c`): dark/purple desktop + `SOVEREIGN MODE` label, keyboard-driven (gate `smoke-compositor`). Full glass/OKLab/animation spec deferred. |
| MANUAL MODE UI | 🟡 IN PROGRESS | 7 | Same compositor renders the light/teal `MANUAL MODE` desktop; `m` key flips to it (gate `smoke-compositor`). Full visual spec deferred. |
| Mode Toggle Animation | 🔴 NOT BUILT | 7 | 300ms cubic-bezier — visual polish, after the compositor renders the static toggle. |
| Quantum Abstraction Layer | 🔴 NOT BUILT | 8 | future; citation unverified |
| AVX-512 memcpy (asm) | 🔴 NOT BUILT | 2 | with CPUID fallback |
| Syscall Entry (asm) | 🟢 COMPLETE | 2e | `arch/x86_64/syscall_entry.asm` — stack switch, save/restore, marshal, SYSRET. |
| IPC Zero-Copy (asm) | 🔴 NOT BUILT | 2d | VMOVDQU |

## ⏸ DEFERRED — tracked, not dropped (interim implementation in place or later phase)

These are intentionally postponed. The current build is functionally complete
without them; each has a concrete "build before" trigger so it is not forgotten.

| Deferred item | Interim / status | Build before |
|---------------|------------------|--------------|
| **APIC** (Local + I/O APIC, APIC timer) | **LAPIC + APIC timer DONE (DDR-714 stage A)** — tick on vector 48, PIT masked; 8259 retained for device IRQs | I/O APIC + MSI (stage C); SMP AP boot (stage B) |
| **3-lane NAS scheduler** (Determ./Throughput/Interactive + AI-hint) | round-robin placeholder (ADR-008) | differentiated agent workloads (Layer 6) |
| **Physical Frame Oracle** (variable-weight + predictive coalesce) | buddy PMM interim (ADR-003) | Sovereign Memory Pool / hugepages (Layer 6) |
| **Agent Virtual Address Space (AVAS)** (128 TB + RO state mirror) | — | AETHER bringup (Layer 6) |
| **Sovereign Memory Pool** (NUMA-pinned hugepages for weights) | — | AETHER/Ollama (Layer 6) |
| **User W^X / NX + guard pages** | ✅ DONE (5a, ADR-021): EFER.NXE on, per-segment perms, guard pages, ring-3 fault → clean kill | — |
| **Kernel-self W^X** (kernel text RX, kernel data NX) | bootloader maps the kernel image RWX; user W^X is fully enforced | kernel-hardening pass (remap kernel sections after boot) |
| **COW fork** | none yet (fork lands in 5b as copy-all-pages; COW after) | memory-heavy fork workloads |
| **Process reaping / AS teardown on exit** | `vmm_destroy_address_space` exists + used on load-failure paths; an exited user process leaks its AS + kernel stack until a reaper | PID-1 orphan reaper (5d) |
| **Dynamic linking** (`ld-pradyos.so`) | static ELF only | shared libraries |
| **UEFI / OVMF boot path** | legacy BIOS/MBR path complete | ARM64 / RISC-V variants (Layer 1) |
| **AVX-512 asm + IPC zero-copy (VMOVDQU)** | scalar paths | performance-hardening pass |
| **Larger/relocating kernel loader** | Stage 2 loads 256 KiB (8×64 sectors) to 0x10000 | kernel exceeds ~256 KiB, or moving the kernel off low memory |
| **virtio: MSI-X + multi-request** | INTx; one in-flight request **per device**, multi-disk via per-device serialization (ADR-015) | APIC live; concurrent in-flight I/O (request-tag table) |
| **Concurrent multi-thread block I/O** | self-tests share the block layer but the driver is single-in-flight-per-disk; per-mount FS lock not yet added | many threads issuing FS/block I/O at once (per-mount + block-layer locks, ADR-016) |
| **SMP spinlocks** | single-core; PMM/console/scheduler use interrupt masking (ADR-016) | second CPU brought online (APIC) |
| **SOVEREIGN FS (SFS) engine** | format + mount + empty-root only (ADR-018 slice 1) | B+ tree insert/lookup, file extents, journalled atomic tx, snapshots, 4 KiB tags, inline LZ4, free-space tree |
| **FAT32 LFN / timestamps** | 8.3 names; new entries get a zero date (no RTC) | long filenames; real mtimes (needs an RTC driver) |

See the per-item rows above for the primary (non-deferred) component status.
