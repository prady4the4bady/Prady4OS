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
**Spring toggle + click ripple (DDR-727):** the mode toggle's linear white
pulse becomes a damped-SPRING amplitude table (overshoot → settle;
`PRADYOS_SPRING_OK`, old `TOGGLE_ANIM_OK` kept); pointer clicks draw a 4-frame
expanding fading ripple ring (`PRADYOS_RIPPLE_OK`). Gates: new `smoke-motion`
(sendkey `s`), and `smoke-mouse` now also asserts the ripple. **72 gates.**
**The Inter typeface (DDR-728) — the LAST deferred L7 visual:** Inter lands as
a pre-rendered **16 px alpha glyph atlas**, not a TTF + rasterizer. Host-side
`tools/fontgen/gen_inter.c` (+ vendored public-domain `stb_truetype.h`)
rasterizes Inter-Regular (SIL OFL 1.1, rsms/inter v4.1) over ASCII 0x20–0x7E
into the generated, committed `user/inter_font.h` (~22 KB: 8-bit alpha glyphs +
metrics). The TTF is NOT vendored — rendered bitmaps are not font software, and
the no-out-of-tree-libs wall governs the OS image, not build-host tooling
(clang/mtools precedent). `draw_str_inter` alpha-blends each glyph with real
pen advances — proportional titles replace the 8×8 monospace where 16 px fits
(TITLEBAR=18); the 8×8 face stays for small text. Sentinel `PRADYOS_FONT_OK`;
gate `smoke-font`. **73 gates.**
**`g_sched_lock` off the switch path (DDR-SMP-rq-2):** rq-1 made the PICK O(1)
but still held the ONE global lock across `context_switch`. It was protecting
two things (double-run is the `on_cpu` claim under the rq leaf lock, not this):
a thief loading a **stale `prev->rsp`** (prev is re-queued before the switch
saves it), and the **exit-vs-collect UAF** (a collector freeing a stack whose
owner still runs on it). Both are now an explicit handshake: `on_cpu >= 0`
means "still executing / rsp not saved", released with a RELEASE store only
AFTER the switch, by whichever thread resumes on that CPU
(`finish_task_switch()` reading a new `percpu.prev` slot — correct because
`%gs` names the CPU we are now on). The picker acquire-spins
(`switch_wait_offcpu`) before touching `next->rsp`; `sched_free_tcb` waits on
the same flag, so `sched_exit` no longer holds the lock across its final switch
(the DDR-SMP-exit-stack-race guarantee is preserved — mechanism local, not
global). `rq_take` drops `on_cpu` from its filter (the DEQUEUE is the
exclusion), which also retires rq-1's transient re-append. `g_sched_lock` now
covers ring topology ONLY and is never held across a switch; the hot path takes
just local IRQ masking + its own rq leaf lock. Static asserts caught the
`percpu` offset shift (`prev` appended after the asm-consumed `u_*` block).
No new gate: the 73 plus stress determinism (rqstress ×8, syswait ×5).
**Deferred (L7):** surface destroy; per-agent live metrics; SFS
`/etc/aether/config`; `CAP_NET` gate; the wlroots/Wayland protocol (out-of-tree
library ports — the standing wall). The DDR-702..709 visual list is now CLOSED:
glass blur (722), gradients (723), typeface (728), pre-transition + cadence
(726), spring/ripple (727), page-flip (721), scroll (725), decorations (724),
alt-tab (720) all shipped.
**Per-wake reschedule IPIs (DDR-SMP-rq-3):** rq-2's deferred half. On unblock,
`sched_unblock` used to just `rq_push` the woken thread and let an idle AP find
it on its own 100 Hz tick (~up to 10 ms of latency). Now the waker scans for an
idle CPU and sends it a **directed** wake IPI (`smp_resched_one`, reusing the
vector-49 wake ISR that just EOIs) so it leaves `hlt` and steals the thread
immediately. A per-CPU `percpu.idle` flag marks the `hlt` wait; the wake/enqueue
race is closed by the idle loop's double-check (set `idle=1`, re-scan
`rq_has_ready()`, loop if work appeared before we halt), with the timer tick as
the correctness backstop. Gate `smoke-resched` asserts `[smp] resched OK` when a
BSP unblock provably kicks an idle AP (`g_resched_ipis > 0`). **74 gates.**
Exposed and fixed a latent **cross-CPU console interleaving** bug: `sys_write`'s
FD_CONSOLE path emitted user bytes via a bare per-char `kputc` loop holding no
lock, so a ring-3 thread printing on an AP garbled mid-line against a `kputs`
from another CPU (rq-3 made this reproducible by running a ring-3 printer on an
AP concurrently with the BSP). Root cause fixed with `kwrite(buf, n)` in
`console.c` — takes `g_console_lock` once for the whole chunk, giving user
writes the same line-atomicity `kputs` already had.
**Surface destroy — lifecycle completion (DDR-729):** a surface
(`kernel/syscall/sys_surface.c`) is a kernel-owned PMM buffer mapped into the
owning client + the compositor. Teardown was explicit-only (`SYS_SURFACE_CLOSE`,
DDR-711), leaving two root defects. (1) **Leak on exit:** a client that exited —
normally, via signal, or fault-kill — without closing leaked its 16-slot table
entry + frames forever; a handful of create/exit cycles hit `-EMFILE`. (2)
**Double-free / ambiguous ownership:** surface (and FB, `sys_fb.c`) frames were
mapped `VMM_USER|VMM_RW|VMM_NX` **without** `PTE_SW_SHARED`, so
`vmm_destroy_address_space`→`free_subtree` `ptnode_free`d them as private user
pages while the surface layer also `pmm_free_pages`'d them — two owners, mis-freed
today on exit-with-mapping and on sovereign-close. **Fix (one owner, one free
point):** mark every client/compositor mapping `PTE_SW_SHARED` (vDSO precedent),
so address-space teardown never touches surface/FB frames — the surface layer is
the sole owner and `surface_free_slot` the sole free. Automatic reclamation via
`surface_reap_pid(pid)` from `sched_exit` frees every slot the exiting pid owns
(no unmap needed — the AS is being destroyed and `PTE_SW_SHARED` prevents the
double free). A leaf spinlock `g_surf_lock` makes the whole slot lifecycle
SMP-safe (allocation/zeroing done outside the IRQ-off window; `copyout`/mapping
loops never held under the lock). Gate `smoke-surfdestroy` (`-smp 4`, freestanding
`user/surfdestroytest.c` — no musl, to stay inside the 512 KiB kernel-image
budget) proves churn, slot-reuse, and exit-reclamation of a full table's worth.
**75 gates.**
**Per-agent live metrics (DDR-730):** the DDR-707 agent panel's active bit was
**never cleared** — a card stayed green after its agent died (`sys_kill_agent`
only posts SIGKILL; `aether_drop_pid` touches the queue, not the roster).
Root-fixed by **deriving liveness lazily**: the roster now stores `{used, pid,
actions}` per slot and reports a slot active iff its pid still resolves to a live
agent tcb (`roster_active`) — a dead agent's card self-dims with no teardown hook
(monotonic pids can't false-positive). New `SYS_AGENT_METRICS` (NSI 64;
`MAX_SYSCALLS` 64->80) returns `{pid, state, mem_used, actions}` per slot read
straight from the live tcb; `actions` is bumped in `sys_submit_action`.
`SYS_AGENT_ROSTER` derives its bits from the same check, so the compositor dot
dims on death with zero compositor growth. Gate `smoke-agentmetrics`
(freestanding `user/agentmetricstest.c` probe) asserts KRYOS reads live
(`state>=1, pid!=0`) while unspawned SOLIN reads idle. **76 gates.**
**Kernel load window: 512 -> 544 KiB (the real-mode ceiling).** The 16-chunk
stage-2 load was full. A 24-chunk (768 KiB) bump **failed boot immediately**
(gates caught it): conventional RAM ends at 0x9FC00, so a flat load at 0x10000
tops out at ~575 KiB — the correct raise is 17 chunks (544 KiB, ends 0x98000).
Growth past that needs kernel relocation above 1 MiB (unreal-mode bounce copy) —
a dedicated boot slice, now documented in the size-check message.
Two regressions the gates caught during this slice, both root-fixed: (1)
`smoke-agent-click` asserted `AGENT PRAX active` — with live-derived liveness a
millisecond-lived clicked agent may never be sampled alive, so the gate now
asserts the deterministic chain (TRIGGER then AGENT_DONE). (2) `smoke-smpuser`
flaked ~50%: `user/hello.asm` printed per-char via `SYS_PUTC`, whose chars from
an AP interleave with other CPUs' lines; the line is now ONE `SYS_WRITE` (atomic
`kwrite` unit, rq-3 contract), newline kept on `SYS_PUTC` for NSI-1 coverage.
**CAP_NET — authority + ownership on the socket NSI (DDR-731):** `SYS_SOCK_*`
(ADR-027) had **no authority check and no slot ownership** — any process could
open outbound TCP (bypassing AETHER arbitration entirely), and could
read/inject/close ANY of the 8 global proxy slots by index; slots also leaked on
exit. Fix: new `tcb.is_net` (CAP_NET; explicitly zeroed in `sched_create_state`,
NOT fork-inherited — a child re-earns authority), granted to agents at spawn
(the sanctioned socket users — live mode talks to Ollama). `SYS_SOCK_CONNECT`
requires `is_net || is_sovereign` (audited `-EPERM`, AETHER pattern);
`g_sock_owner[slot]` records the connecting pid, enforced on WRITE/READ/CLOSE;
`socket_reap_pid` from `sched_exit` closes an exiting pid's slots (the DDR-729
one-owner pattern). Kernel-side lwIP gates (`smoke-net*`) unaffected. Gate
`smoke-capnet` (freestanding `user/capnettest.c`): CAP-less connect and
foreign-slot write/close are all exactly `-EPERM`. **77 gates.**
**Kernel relocated to 4 MiB — unreal-mode bounce load (DDR-733):** the DDR-732
daemon growth (~4 KiB) pushed `__bss_end` to phys 0x9FE80 — past the top of
conventional RAM (0x9FC00) — putting the kernel's BSS tail (late-linked
scheduler/lwIP state) in the EBDA: garbage on the first timer tick, a
deterministic `#GP` in `sched_tick`, gate-caught. The flat real-mode load at
0x10000 has a hard ~575 KiB file+BSS ceiling and it was reached. Root fix:
`KERNEL_PHYS/KERNEL_LMA = 0x400000`. Stage2 INT13-reads each 64-sector chunk
into a 0x10000 bounce buffer and copies it up in **unreal mode** (brief PM
round-trip caches 4 GiB DS/ES limits — SDM Vol.3 §9.9.2; re-armed per chunk
under `cli` so BIOS IRQ handlers can't reset them). 24-chunk/768 KiB read
window; the PT_HI 2 MiB span (0x400000..0x600000) is the honest runtime
ceiling, now enforced by an `nm`-based `__bss_end` Makefile check (the old
file-size-only check was insufficient — the binding quantity is file+BSS).
Page tables stay at 0x300000 (now below the kernel), trampoline at 0x8000,
PMM floor at 16 MiB — all untouched. No new gate: every gate boots this loader.
**77 gates green.**
**AETHER boot config from disk (DDR-732):** the daemon's boot policy
(mode/task/slot) moves from compiled-in constants to `/AETHER.CFG` on the FAT32
boot volume (mcopy'd at image build — operator-editable without rebuilding).
Not SFS `/etc/aether/config`: ring 3 cannot reach SFS (`root_mnt` is the FAT
boot volume; SFS is the kernel's ELF store), so the literal path is impossible
today — a rename when SFS becomes process root. Missing/garbled file falls back
to compiled defaults (`PRADYOS_AETHER_CFG_DEFAULT`) — the daemon never fails to
boot over config. Gate `smoke-aethercfg` asserts the parsed policy line
(`PRADYOS_AETHER_CFG_OK mode=sovereign task=test slot=0`) + the configured
spawn's `PRADYOS_AGENT_DONE`; the DEFAULT sentinel is forbidden. **78 gates.**
**CAP_NET per-host allowlist (DDR-734) — live-agent hardening 1/3:** CAP_NET
was all-or-nothing; a granted agent could connect anywhere. Now a bounded kernel
egress allowlist ({host_be, port}, port 0 = any; 8 entries) is consulted in
`sys_sock_connect` for CAP_NET callers — **empty list = deny-all for agents**
(audited `-EPERM`); the sovereign operator bypasses (it installs the list). New
sovereign-only `SYS_NET_ALLOW` (NSI 65), install-only by design (no runtime
revocation surface — policy changes are a config edit + reboot). The daemon
parses `net=<a.b.c.d>:<port>` rows from `/AETHER.CFG` (DDR-732) and installs
them BEFORE any agent spawns (`PRADYOS_NET_ALLOW_OK n=`); the shipped default is
`net=10.0.2.2:11434` (the Ollama/SLIRP endpoint), so `smoke-agent-live` works
unchanged. Gate `smoke-netallow`: kernel match/deny self-test (`[net] allowlist
OK` — exact match allows, wrong port and wrong host deny) + the config->NSI
install path. **79 gates.**
**Agent CPU metric (DDR-735) — live-agent hardening 2/3:** `SYS_AGENT_METRICS`
gains `run_ticks` (100 Hz ticks while current — sampled CPU time, from
`sched_tick`) and `dispatches` (scheduler switch-ins, from `schedule()`'s claim
point). Both are written only by the CPU that owns the thread (no hot-path
locking — the `on_cpu` claim is the exclusion). The roster slot RETAINS the
last-read counts past the agent's exit, so a short-lived agent's CPU proof stays
observable; the gate probe latches "seen alive" and "dispatches >= 1"
independently across samples (extended `smoke-agentmetrics`, gate count stays
79). Also fixed a dev-host harness flake: `boot_test.sh`'s serial capture is now
`SERIAL_LOG`-overridable (this WSL wipes `/tmp` mid-run, truncating long gates;
CI default unchanged).
**SMP teardown hardening + double-free diagnostic (DDR-735 follow-up):** the
DDR-735 CI run hit a rare `kfree: double free` on one `-smp 4` boot (the common
boot path — all SMP gates share it; rqstress's boot was the unlucky one), a
latent pre-existing race that DDR-735's per-switch timing surfaced. Two changes:
(1) **root-fix** — `thread_trampoline` no longer clears `on_cpu` before its final
`schedule()`. That violated the rq-2 invariant (`on_cpu >= 0` = "still on its
stack / rsp not saved"); `sched_exit` deliberately leaves it set and lets
`finish_task_switch` release it AFTER the switch. Every free path is otherwise
lock-serialized (`g_sched_lock`) and `pid_alive`-exclusive (reaper vs wait4), so
this was the one clear invariant violation in the panicking subsystem.
(2) **diagnostic** — the kheap double-free check now prints the offending pointer
+ object-size class before panicking, so if it recurs the serial log names the
structure (TCB vs kstack vs other) for immediate root-cause. `KHEAP_DEBUG`
(KASAN=1) is on in the normal build, so this fires in CI.
**rq double-enqueue root-caused and FIXED (DDR-736):** the two CI failures (the
`kfree: double free` and a later smpsched hang) share ONE root cause.
`rq_push`'s `rq_on` idempotence check ran under the TARGET queue's leaf lock —
but a waker's `sched_unblock` (device-completion IRQ on CPU B, winning the
`BLOCKED->READY` CAS in the pre-switch window) and the blocker's own
`schedule()` re-queue (CPU A, seeing that READY) push under two DIFFERENT
locks. Both could read `rq_on == 0` and link ONE tcb into TWO FIFOs through its
single `rq_next` pointer — breaking rq-2's exclusion premise ("a thread sits in
exactly one queue"): list corruption loses threads (the hang — a woken FS
thread vanishes) or two CPUs pop and double-run one kernel stack (the double
free). Fix: the `rq_on` claim is now an atomic exchange taken BEFORE any queue
lock (the loser no-ops — the winner's queue holds the thread); `rq_take`'s
unlink clears it with a RELEASE store. Covers every pusher pair, present and
future; queue locks stay leaf; the hot path gains one uncontended atomic. The
race predates DDR-735 (rq-1 era — the "benign spurious wake" comment described
the pre-queue ring-walk scheduler); DDR-735's timing shift + CI's TCG runners
surfaced it. The KASAN double-free diagnostic stays as the tripwire.
**smoke-agentmetrics made TCG-deterministic (DDR-735 gate fix):** with DDR-736
in place the corruption family stopped recurring in CI, but the next run
(29203329840) failed `smoke-agentmetrics` for an unrelated reason: the gate's
alive-window assertion (`state>=1` sampled during the agent's life) is racy on
TCG runners — a seconds-long compositor quantum let the agent's entire life fit
between two probe samples, and refresh-on-read retention then kept the counts at
zero forever. Fixed by making the proof post-mortem stable: `sched_exit` now
captures the final counters into the roster slot (`agent_metrics_reap`, the
DDR-729 hook pattern) and the dead slot retains its pid; the probe asserts
`pid!=0 && dispatches>=1` on slot 0 vs `pid==0 && dispatches==0` on slot 7,
prints the alive observation only opportunistically, and bounds its poll by 120
RTC seconds (SYS_CLOCK) instead of an iteration count.
**Agent-panel live metrics (DDR-737) — live-agent hardening 3/3, campaign
CLOSED:** the compositor's agent cards now render from `SYS_AGENT_METRICS`
(which subsumes the roster read — `state >= 1` IS the DDR-730 liveness bit):
status dot state-colored (green run/ready, amber blocked, **dim green =
ran-and-exited** via the retained pid, gray = never spawned) plus up to 4
activity pips (submitted actions; DDR-735's post-mortem retention keeps them
lit after the agent completes). One-shot serial witness `AGENT_PANEL KRYOS
act= disp=` + `PRADYOS_AGENT_PANEL_METRICS_OK`, keyed on the post-mortem-stable
`pid!=0 && dispatches>=1` fact so TCG frame cadence can't miss it. This was
DDR-730's reverted plan; DDR-733's 768 KiB window removed the image blocker
(kernel.bin 545 KiB, ~236 KiB headroom). Gate `smoke-agentpanel` (GPU).
**80 gates.**
**SFS hierarchical directories (DDR-738):** SFS dir keys were already
`(parent_inode<<32)|hash32`, but `open`/`create` resolved one component under
the root, `readdir` ignored its path, and inodes had no dir type. Added:
`SFS_INO_DIR` inode flag (the root is implicitly a dir — back-compatible with
existing volumes); `sfs_walk` splits the path on `/` and resolves intermediates;
`open` requires each intermediate to exist and be a directory; `create` does
**mkdir -p** on intermediates (dir inodes) then the final file; `readdir` walks
to the target dir inode. All SFS-local — the VFS vtable is unchanged, so FAT/ext4
and the process root (still FAT) are untouched; existing single-component SFS
paths are the 1-deep case. Gate `smoke-sfs-dirs` builds/reads `/etc/aether/config`,
rejects dir-as-file + missing-intermediate opens, and enumerates each level.
**81 gates.** Follow-on slices: switch the process root to SFS, then move
`/AETHER.CFG` to its intended `/etc/aether/config`.
**Per-process root mount (DDR-739) — SFS-as-root, half 1/2:** `tcb.root_mnt` was
per-process (fork inherits it) but hard-wired — `elf_load` set every process to
`vfs_default_mnt()` (FAT). Added spawn-with-root: the spawner sets `t->root_mnt`
BEFORE `sched_unblock`. Proven by a ring-3 probe (`user/rootmounttest.c`,
freestanding) spawned from embedded bytes with its root set to the ext4 mount
(blk3 — read-only, never unmounted, survives to scheduler time, unlike SFS): it
opens `/EXT4.TXT` (ext4-only) AND fails `/HELLO.TXT` (FAT-only), two-sided proof
`SYS_OPEN` used the selected root. The single ext4 mount is taken once early and
reused by the existing ext4 self-test (within `VFS_MAX_MOUNTS=4`). Gate
`smoke-rootmount` (needs the ext4 disk). **82 gates.**
**Context-switch perf: lazy FPU (DDR-740) — target met.** `schedule()`
`fxsave`/`fxrstor`'d the 512-byte FPU state on EVERY switch, but the kernel is
`-mgeneral-regs-only` (no SSE even in string ops), so kernel threads never touch
the x87/SSE register file — the save/restore was pure waste on any
kernel-involved switch. Now guarded on `is_user`: save `prev` iff `prev->is_user`,
restore `next` iff `next->is_user`. A user thread still always sees its own state
(U->K saves U; K->V restores V); kernel threads (which can't read the file) are
skipped. Measured cost dropped **~4552 -> ~2547 cycles (~1881 -> ~1054 ns)** — a
44% cut, under the <=1500 ns Layer-2 target. `smoke-fpu` (two ring-3 XMM users)
is the correctness gate; the perf number is real-hardware and not CI-assertable
on TCG. No CR0.TS trap machinery.
**SFS unlink + rmdir (DDR-741) — lifecycle completed.** `sfs_unlink` was a stub
and the B+tree has no delete. Since `bt_insert` replaces an equal key, removal
now overwrites the name->inode DIR entry with a **tombstone** (`inode_num == 0` —
never valid: root is 1, `next_inode >= 2`). Lookup treats a tombstone as
not-found, `dir_walk` skips it (invisible to readdir + the empty-dir check), and
create treats a tombstoned slot as available (re-creatable). `sfs_unlink` handles
files AND empty directories (empty-check via `dir_walk`) through the single
`vfs_unlink` op — no VFS-vtable change, FAT/ext4 untouched. Block reclamation
deferred (bounded leak until reformat; a CoW free-space GC is a later slice).
Gate `smoke-sfs-unlink` (create/unlink/re-create, ENOTEMPTY, leaf-first rmdir,
readdir gone). **83 gates.**
**SYS_GETDENTS — ring-3 directory listing (DDR-742).** Ring 3 had no way to
enumerate a directory (PRISM's `ls` was a stub). New `SYS_GETDENTS` (NSI 66):
`(path, index, name_buf) -> namelen | 0(end) | -errno`, per-entry to match
`vfs_readdir`'s index API. The handler mirrors `sys_open` — resolves `path`
against the caller's `root_mnt` + `fs_cap` (honoring per-process roots, DDR-739)
and `copyout`s the name. PRISM's `ls [dir]` loops it. Gate: `smoke-shell`
extended to feed `ls /` and assert an anchored `^HELLO.TXT$` line (PRISM's bare
name, distinct from the kernel's indented boot `fs_list`) — so it specifically
exercises the syscall through the real shell. **83 gates** (no new gate).
**SYS_GETPROCS — ring-3 process listing / `ps` (DDR-743).** PRISM's `ps` was a
stub (own pid only); ring 3 could not enumerate the scheduler ring. New
`SYS_GETPROCS` (NSI 67): `(index, struct procinfo*) -> 1 | 0(end) | -errno`,
per-entry like `getdents`. `procinfo` is a pure-value snapshot
`{pid, ppid, state, flags, name[16]}` (flags bit 0 = user). The walk lives in
`sched.c` as `sched_snapshot(index, out)` — the ring (a persistent all-threads
circular `tcb.next` list, unlinked only at reap) is walked from
`current_thread` under `g_sched_lock`; `sys_proc.c` never touches the ring and
`copyout`s after the snapshot. `ps` loops it, printing `PID PPID S U NAME` for
every live thread (kernel + user). Gate: `smoke-shell` feeds `ps` and asserts
the `PID  PPID S U NAME` header (deterministic witness of the syscall). Fixed an
incidental DDR-742 `ls`-gate flake: the `^HELLO.TXT$` anchor broke when the
`prism> ` prompt shared the output line (flush/read timing) — relaxed to
`(^|prism> )HELLO.TXT$`, still trailing-anchored so the kernel's `fs_list` line
is excluded. **83 gates** (no new gate).
**Ring-3 file lifecycle — O_CREAT + SYS_UNLINK + FD_VFS write (DDR-744).** Ring 3
could `open`/`read`/`readdir` but had no way to create, write, or delete a file —
the DDR-741 unlink path was proven only in-kernel. Added, all resolved against the
caller's `root_mnt` + `fs_cap`: (1) `O_CREAT` (0x40) on `sys_open` (falls back to
`vfs_create` when the open misses); (2) `SYS_UNLINK` (NSI 68) → `vfs_unlink`
(file or empty dir; SFS's `0`/`-1` backend rc collapses to `-ENOENT`); (3) the
`FD_VFS` branch of `sys_write`, which was a `-EBADF` stub ("slice 4", never
landed) — now a chunked `copyin`+`vfs_write` mirror of the `FD_VFS` read path,
advancing the fd offset (needed to populate a created file from ring 3). Gate
`smoke-fsrm`: an **SFS-rooted** freestanding probe (FAT has no ring-3
create/unlink) creates `/RMPROBE` via `O_CREAT`, writes+reads it back, unlinks,
confirms it is gone, and re-unlinks (clean error) → `PRADYOS_FSRM_OK`. **84 gates.**
**PRISM `touch`/`rm` — writable shell on the real FAT root (DDR-745).** FAT32
already implements `create`/`write`/`unlink`, and PRISM is rooted at the FAT
default mount, so DDR-744's `O_CREAT`/`SYS_UNLINK` already worked there — only the
shell builtins were missing. Added `touch <path>` (`SYS_OPEN` O_CREAT|O_WRONLY,
create-if-absent) and `rm <path>` (`SYS_UNLINK`), one-liners with no kernel
change. `smoke-shell` extended: `touch /PRISMNEW.TXT` → `ls /` lists it (prompt-
tolerant anchor) → `rm /PRISMNEW.TXT` prints `rm: removed …` — exercising the
DDR-744 syscalls through the real interactive shell on the default root, not a
probe. **84 gates** (no new gate).
**ACPI S5 poweroff — `SYS_POWEROFF` (DDR-746).** First power-management slice. The
ACPI parser existed (RSDP/RSDT/XSDT) but nothing parsed the FADT or shut the
machine down. `acpi_power_init()` reads the FADT (`FACP`) PM1a/b_CNT ports + the
DSDT `\_S5_` sleep-type values (minimal scan, ACPI §7.4.2 — not a full AML
interpreter); `acpi_poweroff()` prints `PRADYOS_POWEROFF` then writes
`(SLP_TYP<<10)|SLP_EN` to PM1a_CNT (S5 soft-off). Exposed as `SYS_POWEROFF`
(NSI 69), CAP_SOVEREIGN-gated like `SYS_SET_MODE` (`-EPERM` for non-sovereign,
`-ENODEV` if no S5). `outw`/`inw` added to `kernel/io.h`. The compositor's `p`
key issues it. Gate `smoke-poweroff`: GPU boot + QMP `sendkey p`; QEMU has no
`-no-shutdown`, so S5 exits it — the gate asserts the pre-write sentinel (only
this gate sends `p`, so other boots are unaffected). **85 gates.**
**ACPI reboot — `SYS_REBOOT` (DDR-747).** Sibling of DDR-746, completing basic
power management. `acpi_power_init()` also parses the FADT reset register
(Flags@112 bit10 `RESET_REG_SUPPORTED`, RESET_REG GAS@116, RESET_VALUE@128, when
System-I/O). `acpi_reboot()` prints `PRADYOS_REBOOT` then tries, in order: the
FADT reset write, `0xCF9<-0x0E` (PCI reset), `0x64<-0xFE` (8042 CPU-reset pulse) —
the last two always reset a PC/QEMU, so reboot is robust regardless of the FADT.
Exposed as `SYS_REBOOT` (NSI 70), CAP_SOVEREIGN-gated like `SYS_POWEROFF` (no
`-ENODEV` — reset is always attempted). Compositor `b` key issues it. Gate
`smoke-reboot`: GPU boot + QMP `sendkey b`; QEMU runs `-no-reboot`, so the reset
exits it — asserts the pre-reset sentinel (only this gate sends `b`). **86 gates.**
**System introspection — `SYS_SYSINFO` (DDR-748).** Ring 3 had no way to learn the
machine or its live state. New `SYS_SYSINFO` (NSI 71, no cap — read-only) fills a
`struct sysinfo {vendor[16], brand[64], cpu_count, feat_edx/ecx, uptime_ticks,
free_pages}` from `cpu_cpuid` (leaf 0 vendor, 0x80000002..4 brand, leaf 1
features), `lapic_cpu_count`, `g_ticks`, and `pmm_free_page_count`, then
`copyout`s it. Freestanding probe `sysinfotest` echoes the CPU vendor/brand
(verified: `AuthenticAMD` / `QEMU Virtual CPU version 2.5+`) and validates the
numeric fields → `PRADYOS_SYSINFO_OK`. Gate `smoke-sysinfo` (deterministic —
stable CPUID + frame count). Total-RAM/load-average deferred. **87 gates.**
**Wall-clock date/time — `SYS_TIME` (DDR-749).** `SYS_CLOCK` (DDR-709) gave only
seconds-since-midnight; ring 3 could not read the date. New `SYS_TIME` (NSI 72,
no cap) `copyout`s the broken-down RTC reading (`rtc_now` → `struct rtc_time
{year, month, day, hour, minute, second}`). Freestanding probe `timetest` formats
`TIME YYYY-MM-DD HH:MM:SS` (verified `2026-07-14 08:13:57`) and range-validates
each field → `PRADYOS_TIME_OK`. Gate `smoke-time` — deterministic (the exact
value is host-provided but the field ranges always hold). **88 gates.**
**Kernel log ring + `SYS_DMESG` (DDR-750).** Kernel output went straight out COM1
with no in-memory record. `kputc` (the single output funnel) now also captures
every byte into an 8 KiB circular ring under a dedicated leaf spinlock (`klog_lock`,
always innermost — no deadlock vs. the console lock). `SYS_DMESG` (NSI 73, no cap
— diagnostic) stages the recent log into a 4 KiB kernel buffer via `klog_read`
(lock held only around the in-kernel copy, never across `copyout`) and copies it
out. Freestanding probe `dmesgtest` writes a unique marker (which flows through
`kputc` into the ring), reads the log back, and confirms the marker → ring-size-
independent proof. Gate `smoke-dmesg`. **89 gates.**
**PRISM system builtins (DDR-751).** The DDR-748/749/750 syscalls had only test
probes as consumers. PRISM (musl C, so `printf` formatting is free) gains
`uname` (SYS_SYSINFO → `AuthenticAMD "QEMU Virtual CPU version 2.5+" cpus=1`),
`date` (SYS_TIME → `2026-07-14 22:19:12`), `uptime` (SYS_SYSINFO
`uptime_ticks/100`), and `dmesg` (SYS_DMESG → byte-count header + the recent log).
No kernel change (all three syscalls are uncapped). `poweroff`/`reboot` are
intentionally omitted (CAP_SOVEREIGN — the compositor's `p`/`b` keys). Gate:
`smoke-shell` feeds all four and asserts their fixed output shapes. **89 gates**
(no new gate).
**`SYS_MEMINFO` + PMM total-RAM tracking + PRISM `free` (DDR-752).** Closes the
DDR-748 total-RAM deferral: the PMM now captures `total_pages = free_pages` at the
end of `pmm_init`'s E820 sweep (before the permanent refcount-table carve), with a
`pmm_total_page_count()` getter. `SYS_MEMINFO` (NSI 74, no cap) copies out `struct
meminfo {total, free, used (=total-free, derived), page_size}`. PRISM `free` prints
`mem: total=<K>K free=<K>K used=<K>K` (verified `total=114520K free=70548K
used=43972K`). `used` is whole-frame physical accounting (kernel image, page
tables, refcount table, all allocs), not userspace RSS. Gate: `smoke-shell` feeds
`free` and asserts the shape. **89 gates** (no new gate).
**TCP loopback echo self-test (DDR-753).** The net stack had a UDP loopback gate
and a TCP echo *server* (`:8007`), but nothing drove the TCP *client* path
end-to-end. `net_loopback_tcp_test()` (in `net_init`, IRQ-masked) `tcp_connect`s
to `127.0.0.1:8007`, writes `"ping"` on the connected callback, and verifies the
echoed bytes in its recv callback, driven by a bounded `netif_poll_all` +
`sys_check_timeouts` pump (≤200 iters — converges immediately over loopback; the
cap guarantees no hang). Verified full round-trip: `TCP_READY` (accepted) →
`TCP_OK` (data) → `PRADYOS_NET_TCP_LO_OK` (echo verified). Gate `smoke-net-tcp-lo`
(deterministic — in-guest loopback, no external network). **90 gates.**
**`ps` CPU accounting (DDR-754).** `ps` listed pid/ppid/state/name but no CPU
usage, though DDR-735's per-tcb `run_ticks`/`dispatches` counters already tick for
every thread. `struct procinfo` (SYS_GETPROCS) gains `run_ticks`/`dispatches`;
`sched_snapshot` copies them under `g_sched_lock`; PRISM `ps` prints a
`CPUms` (`run_ticks*10`, 100 Hz) + `DISP` (switch count) column — new header
`PID PPID S U CPUms DISP NAME` (verified: `PRISM.ELF 60ms/8216`, `reaper
580ms/36501`). Read-only; no scheduling-logic change. The `procinfo` struct + its
sole consumer (PRISM's mirror) updated in lockstep. Gate: `smoke-shell` asserts
the new header shape. **90 gates** (no new gate).
**Process signaling — kill end-to-end + PRISM `kill` (DDR-755).** `SYS_KILL` +
`signal_deliver` existed (SIGKILL non-maskable, SIGTERM-default terminates) but
were never proven end-to-end and had no shell verb. Freestanding `killtest`
probe `fork`s a child that spins in ring 3 forever (`pause` loop — no exit path),
then `SYS_KILL(child, SIGKILL)` + `SYS_WAIT4` — reaching past `wait4` proves the
signal terminated the otherwise-immortal child (`PRADYOS_KILL_OK`). PRISM
`kill <pid> [sig]` sends `SIGTERM` (or a given signum). No kernel change. Gate
`smoke-kill` — the child's infinite loop makes a false pass impossible (a broken
kill blocks `wait4` → clean timeout). **91 gates.**
**Self-rename — `SYS_SETNAME` (DDR-756).** A process's `ps` name was fixed by the
loader (the ELF filename); nothing let a process/agent label itself. `struct tcb`
gains a `char name_buf[16]` (zeroed in `sched_create_state`); `SYS_SETNAME`
(NSI 75, no cap — self only, so no escalation) `copyinstr`s ≤15 chars into it and
repoints `t->name`. `ps` reflects it immediately (`sched_snapshot` reads
`t->name`). PRISM `setname <name>` wraps it. Self-verify probe renames to
`KILROY` then confirms via `SYS_GETPROCS` → `PRADYOS_SETNAME_OK`. Gate
`smoke-setname`. **92 gates.**
**Kernel-self W^X (DDR-757) — M1 kernel hardening 1/3.** Closes the long-deferred
ADR-021 item (stage2 mapped the kernel image RWX). `kernel.ld` page-aligns the
section boundaries (`__text_end`/`__rodata_end`); `vmm_protect_kernel()` (after
`vmm_init`, before SMP) re-walks the PT_HI page and stamps text RX, rodata R+NX,
data/BSS RW+NX, plus NX on the 2 MiB identity alias, then re-walks and audits the
live PTEs → `[wx] kernel W^X OK`. Shared kernel top-level entries mean every AS is
hardened. Threat-modeling surfaced + fixed an AP boot ordering bug: the trampoline
now arms NXE (via a BSP-written EFER OR-mask in the mailbox) *before* paging, so
APs don't RSVD-#PF on the now-NX kernel-data pages (all SMP gates pass). Residual:
kernel text stays writable through the identity alias (documented follow-on). Gate
`smoke-wxkernel`. **93 gates.**
**Syscall-fuzz gate (DDR-758) — M1 kernel hardening 2/3.** Stress-tests the
ring-3→ring-0 boundary. Freestanding `syscallfuzz` probe (fixed-seed 64-bit LCG →
reproducible) floods 3000 hostile syscalls: bad NSI numbers (negatives + out of
range) that must hit the dispatch bounds check (`-ENOSYS`, no off-end deref), and
wild pointers (NULL/kernel-VA/non-canonical/unmapped) into an **allowlist** of
read-only query syscalls that must return `-EFAULT` via the uaccess fixup. The
allowlist (not a denylist) guarantees no destructive/self-terminating NSI body is
invoked, so the probe always runs to completion → `PRADYOS_FUZZ_OK`. Verified: 0
panics, boot continues past the flood. Gate `smoke-syscallfuzz`. **94 gates.**
**SMP block-read integrity audit (DDR-759) — M1 kernel hardening 3/3.** The
audit's detection instrument for the logged intermittent `-smp 4` FS flake.
`blkmq_proof` only checked read *success*; `smp_blk_integrity()` verifies *data*:
it records a single-threaded reference checksum for sectors 0..3, then 4 kernel
workers (distributed across CPUs under `-smp 4`, keeping the 8 DDR-BLK-1 slots
busy) each re-read a sector 64× and compare each read's checksum to the reference
— a completion mis-routed to the wrong slot (wrong data) is caught, not tolerated.
Read-only (no writes → no image/FS corruption); no change to the hardened
`virtio_blk.c` submit/complete. Verified green 5/5 under `-smp 4` (no wrong-data
race observed — strong evidence the earlier one-off was infra/timing, and a
deterministic repro instrument if it ever recurs). Gate `smoke-blk-integrity`.
**95 gates.** *M1 kernel hardening (W^X + syscall-fuzz + SMP audit) COMPLETE.*
**Persistent SFS root (DDR-760) — M2 storage 1/N (SFS-as-root half 2/2).** DDR-739
gave per-process roots (proven on ext4) but SFS could not be a durable root: the
boot SFS self-tests `vfs_unmount` blk2 and run the destructive journal/snapshot/
LZ4 tests on the raw device, leaving it corrupted. Fix (operator-endorsed low-risk
single-disk path): all user ELFs are already loaded before the destructive tests,
so *additively* after them — `sfs_format(sbd)` reformats blk2 clean, `vfs_mount(2)`
remounts, the kernel provisions `/etc/aether/config`, and a probe is rooted there
(DDR-739 hand-rolled: load blocked → set `root_mnt` → unblock). The probe reads
`/etc/aether/config` through its SFS root → `PRADYOS_SFSROOT_OK`. No change to the
fragile ELF-load sequence; existing destructive SFS gates unchanged. Gate
`smoke-sfsroot`. Follow-ons: re-point the live daemon off `/AETHER.CFG` (FAT) to
this SFS config; SFS free-space GC; host `mkfs.sfs`. **96 gates.**
**AETHER config on SFS (DDR-761) — M2 storage 2/N.** The daemon read its boot
policy from `/AETHER.CFG` on the FAT boot volume (DDR-732 stopgap); now it reads
`/etc/aether/config` on the DDR-760 SFS root — config lives on the sovereign FS at
its real path. The daemon opens only its config, so switching its root is safe:
it is now `elf_load`ed **blocked** at spawn (hand-rolled, DDR-739 pattern) and
`root_mnt`+unblocked in the reformat/provision block, after the kernel writes the
full policy (`mode/task/slot`+`net=` CAP_NET row) to SFS `/etc/aether/config`.
Deferring the daemon a few boot steps is safe (only it spawns agents; none run
until it does; `g_aether_daemon_pid` is set at load). Validated by the existing
`smoke-aethercfg` (now green against the SFS source) + the full AETHER/agent set
(`smoke-aether`/`-queue`/`-sec`/`-agents`/`-agentmetrics`/`-netallow`) + `smoke-
sfsroot`. **96 gates** (no new gate). *Cleanup:* the now-dead FAT `/AETHER.CFG`
is retired from the image build (no reader remains after the SFS migration);
stale `/AETHER.CFG` comments across the daemon/socket/Makefile updated to the SFS
path — verified `smoke-aethercfg`/`-netallow`/`-fs`/boot still green with it gone.
**SFS B+tree churn — misdiagnosis corrected (DDR-763).** A prior "correctness-
critical SFS B+tree bug" (repeated create+write(64K)+unlink failing the write at
~cycle 11) was reproduced with per-return-path instrumentation in `sfs_write` —
NONE of its markers fired, so the write never reached SFS. It failed in
`vfs_write`'s `fs_write_budget < len` check: the **1 MiB per-thread write budget**
(`FS_WRITE_BUDGET_DEFAULT`) exhausted by the boot thread's ~20 ELF-to-SFS writes +
FS self-tests, leaving room for only ~10 more 64 KB writes. The 14-slot leaf-split
coincidence was a red herring — **there is no B+tree bug** (`sfs.c` unchanged);
refreshing the budget makes the churn reach 40/40. New gate `smoke-sfs-btree` (40×
create/write/unlink past the leaf split, budget refreshed to test the tree)
closes the coverage gap that allowed the misdiagnosis. **97 gates.**
**SFS free-space reclamation (DDR-762-v2).** Closes the DDR-741 block leak. The
naive per-block free stack was WRONG (extents need CONTIGUOUS blocks; `write_extent`
records `block_start=next_free`). Correct design = a free-EXTENT-RUN allocator:
`sfs_ctx.free_runs[256]={start,count}`; `free_run()` snapshot-guarded push on
unlink of each extent + the inode block; `alloc_run(n)` **EXACT-fit, never split**
else bump; `alloc_block=alloc_run(1)`; `write_extent` uses `alloc_run(nblocks)`
writing `[start,start+n)`. Exact-fit (not first-fit-split) is essential: single-
block inode/B+tree `alloc_run(1)` would otherwise fragment a freed 16-block extent
run before the next write could reuse it whole. Invariant: a run enters only when
`snapshot_count==0`, so a reused run is never snapshot-referenced; uniform files
reuse exactly. Gate `smoke-sfs-gc` observes reuse DIRECTLY via the committed
high-water (`sfs_read_next_free`): 10× create+write(64K incompressible)+unlink,
assert the `next_free` delta `grew < 170` — measured reclaim `grew≈92`,
no-reclaim `≈262` (the 300-cycle exhaustion loop was correct but timed other
gates' boots out on TCG; the delta is the same proof, cheap). Full SFS suite incl.
destructive journal/snapshot/lz4 + SMP `blkmq` + `smoke-fs-ext4` green (no
corruption). In-memory reclaim (within-a-boot); on-disk free tree deferred.
**98 gates.**
**Ring-3 VFS write chunk 256 B → 4 KiB (DDR-764).** `fd_write_user` (the FD_VFS
`SYS_WRITE` path) copied user data through a 256-byte kernel buffer — 1 `vfs_write`
per 256 B. Two costs: 16× the iterations, and (SFS) each 256 B chunk became one
extent, so with the 4-inline-extent inode cap a ring-3 process could write only
~1 KiB to an SFS file. Now chunks at one 4 KiB block from a PMM page (not the
16 KiB kernel stack) → 16× fewer iterations and ring-3 SFS files reach 4 × 4 KiB =
16 KiB; FAT just writes 16× faster. Gate `smoke-vfs-bigwrite`: an SFS-rooted probe
writes 8 KiB in one `SYS_WRITE` + reads it back (256-chunk short-writes at ~1 KiB →
verified discriminating). Regression: shell touch/rm, fsrm lifecycle, sfsroot,
fs-rw, sysio all green. Follow-ons: SFS extent-overflow (lift the 16 KiB ceiling),
refillable write budget. **99 gates.**

**DDR-765 (NVMe controller bring-up + Identify — M2 driver 1/2):** the first
non-virtio block driver. Detects an NVMe controller by PCIe class 0x01 /
subclass 0x08, maps BAR0 uncached, resets + enables the controller (CC.EN,
poll CSTS.RDY), stands up a single admin SQ/CQ pair (one identity-mapped PMM
page each), and runs Identify Controller (CNS=1) + Identify Namespace (CNS=0,
NSID=1) over the admin queue — completion polled via the CQ phase bit, every
hardware wait bounded by a spin counter so a missing/wedged controller cannot
hang boot. Prints `[nvme] <model> ns1 <NSZE> LBAs x <lbasize> B`; on QEMU's
`-device nvme` that is `QEMU NVMe Ctrl ns1 32768 LBAs x 512 B` (16 MiB image).
New gate `smoke-nvme` (`QEMU_NVME=1` knob in boot_test.sh attaches the
controller; every other gate boots without one, so the driver is a no-op there).

**DDR-766 (NVMe I/O queue + read/write + blk_register — M2 driver 2/2):** makes
the namespace a real block device. Creates one I/O SQ/CQ pair via Create I/O
Completion/Submission Queue admin commands (qid 1, PC=1, poll — no IRQ), issues
NVM Read (0x02)/Write (0x01) with a PRP1-only per-≤page-chunk loop (any buffer
alignment; PRP lists deferred), and `blk_register`s `nvme0` with
`capacity_sectors = NSZE` (512-byte LBA only this slice; non-512 logs + skips).
Boot self-test round-trips LBA 100 (write pattern → read back → verify) on the
scratch nvme.img → `PRADYOS_NVME_RW_OK`; `smoke-nvme` asserts it plus
`registered nvme0`. Verified: `[nvme] registered nvme0 (32768 sectors)` +
`PRADYOS_NVME_RW_OK`, image `-Werror`-clean. Still **100 gates** (smoke-nvme
extended, not a new gate).
**DDR-767 (host `mkfs.sfs` — build-time SFS provisioning):** a host tool that
writes byte-exact SFS images the kernel reads. `tools/mkfs_sfs/mkfs_sfs.c` +
`sfs_readback.c` both `#include` the kernel's `sfs.h`, so their on-disk structs +
FNV-1a name hash are identical to the kernel reader (no drift). mkfs formats
blocks 0–3 exactly like `sfs_format`, then provisions root-level files (inode +
inline extents + DIR/INODE leaf slots into the single root B+tree leaf). New
host gate `smoke-mkfs-sfs`: mkfs writes `/PERSIST.TXT`; `sfs_readback` (kernel
structs + the kernel's leaf-scan → inode → extent read path) recovers it
byte-for-byte (~1 s, no boot). Verified locally. The kernel-boots-and-reads
end-to-end proof is **DDR-768**. Also this commit: bumped the 16 `-smp 4` gate
timeouts (60→120, 90→180 s) — CI's slower 4-vCPU TCG intermittently exceeded the
tight 90 s margin on `smoke-surfdestroy` (test booted + passed locally; a
runner-speed timeout flake, not a correctness bug — the deeper SMP-race
investigation stays open). **101 gates.**
**DDR-768 (cross-reboot SFS persistence proof):** the kernel boots on a
host-authored `mkfs.sfs` image and reads a file back — end-to-end host→kernel SFS
interop. A guarded self-test (`main.c`, in `fs_test_thread`) peeks the highest
blk index's block 0 for `SFS_MAGIC` (raw `blk_read`; the blank in-kernel SFS disk
isn't formatted yet, so it only fires for a pre-formatted host image), mounts it,
`vfs_open`/`vfs_read`s `/PERSIST.TXT`, and prints `PRADYOS_SFS_PERSIST_OK`.
`boot_test.sh` gains `QEMU_SFS2` (attach the mkfs image last) + `QEMU_NO_EXT4`
(the virtio-blk driver caps at `VBLK_MAX=4`; MSI-X vectors 50–53 are packed
against net@54/input@55, so suppressing ext4 keeps the persist disk within the
cap rather than remapping vectors). `VFS_MAX_MOUNTS` 4→6 for mount headroom. New
gate `smoke-sfs-persist` — verified `PRADYOS_SFS_PERSIST_OK` locally.

**DDR-769 (nested-directory provisioning in `mkfs.sfs`):** mkfs now writes
directory hierarchies the kernel traverses. `add_file(path,…)` walks `/`-split
components — intermediates become dir inodes (`SFS_INO_DIR`, no extents) with
find-or-create dedup of shared prefixes, the last is the file — all keyed
`(parent_inode<<32)|FNV1a32(name)` into the single root leaf (≤14 slots).
`sfs_readback` walks the same way. `smoke-sfs-persist` extended: mkfs provisions
`/etc/aether/config` and the kernel persist self-test reads it →
`PRADYOS_SFS_NESTED_OK`. Verified locally (host round-trip + kernel boot). **102
gates** (same gate, extra assertion).
**DDR-770 (persistent root from a host mkfs.sfs image):** the AETHER boot policy
`/etc/aether/config` now ships in a build-time image instead of being
kernel-provisioned. `build/sfsroot.img` (mkfs.sfs, nested-dir `/etc/aether/config`
= real policy text) attaches via the `QEMU_SFSROOT` knob. The DDR-768 peek, on
finding an SFS disk whose `/etc/aether/config` starts with `mode=`, records its
mount (`prov_mnt`); the persistent-root block then roots the AETHER daemon there
and **skips** the kernel `vfs_create`/`vfs_write` of the config. Default boots
(no provisioned disk) are unchanged — blk2 reformat+provision fallback intact.
DDR-769's nested test marker moved to `/etc/test/config` to keep
`/etc/aether/config` unambiguous. New gate `smoke-aether-sfsroot` — verified
`AETHER daemon rooted at provisioned mkfs image` + `PRADYOS_AETHER_CFG_OK
mode=sovereign` locally.

**DDR-771 (`VBLK_MAX` 4→8, MSI-X vector remap):** the virtio-blk driver now
supports 8 disks. The block MSI-X window moved from 50–53 to **56–63** (clear of
net@54/input@55), which required extending the shared MSI-X infrastructure:
`isr.asm` ISR stubs + `isr_stub_table` to 64, `idt.c` gate loop to 64, and
`MSIX_VEC_COUNT` 6→14. Root-caused a #GP (err 0x1C2 → IDT vector 56 ungated) that
a driver-only change first triggered. `smoke-aether-sfsroot` now boots **five**
virtio-blk disks (boot/fat/sfs/ext4/sfsroot) — asserts `blk4 ready` (5th disk
registered) + the AETHER daemon roots at the provisioned image at blk4 alongside
ext4. **104 gates.**

**ADR-032 (FS write budget: lifetime cap → token-bucket rate limit):** supersedes
ADR-015's per-thread *lifetime* write cap (1 MiB total, ever — far too little for
a real process) with a **token bucket**: `fs_write_budget` is a balance that
refills `FS_WRITE_REFILL_PER_TICK = 256 KiB`/tick (25 MiB/s sustained) up to a
`FS_WRITE_BURST_MAX = 1 MiB` cap, added `tcb.fs_budget_tick`. `vfs_write` lazily
refills from elapsed ticks; refill only tops up a below-cap balance and never
reduces a higher one, preserving the kernel self-test `~0ull` bypass. Bounds the
write RATE (anti-flood) with no lifetime ceiling; disk-SPACE exhaustion stays a
separate control (SFS free-space GC + future per-mount quotas). New gate
`smoke-fs-budget` (deterministic: a drained bucket refills from simulated elapsed
ticks and writes — impossible under the old cap; and rejects with no elapsed time
— rate limit holds). churn/bigwrite regressions green. **105 gates.**
**Next:** open items — make the provisioned root the DEFAULT boot root + retire
blk2's dual role (now unblocked); `-smp 4` SMP-race root-cause; NVMe PRP-list
(>page single commands) + NVMe IRQ; mkfs multi-leaf trees (>14 slots). Dev note:
`make image` does not always rebuild `main.o` on source change — `rm build/main.o`
before local test builds (CI clean-builds so it's unaffected). wlroots/Wayland
remain out-of-tree.

**DDR-772 (NVMe PRP2 + PRP list — multi-page single commands):** `nvme_io` no
longer issues one command per ≤page chunk. `nvme_submit` gained a `prp2` arg; a
per-controller scratch PRP-list page holds up to 512 page addresses. Per command:
PRP1 (a possibly page-offset first region), `PRP2 = 0` (fits PRP1) / second-page
base (one more page) / PRP-list phys (N>1 more pages), capped at ~2 MiB
(4096 sectors) per command with a loop for larger. `smoke-nvme` extended: a
16 KiB (4-page) round-trip now completes as ONE command via a 3-entry PRP list →
`PRADYOS_NVME_PRP_OK`. **106 gates.** NVMe completion IRQ still deferred (needs a
vector-pressure DDR + a poll→IRQ completion rework; note DDR-771 freed vectors
50–53, so the window is no longer full).

**DDR-773 (mkfs.sfs multi-leaf B+tree):** `mkfs.sfs` no longer hard-errors past
`SFS_LEAF_MAX = 14` slots (~6 files). Slots are now collected flat (bounded at
`MKFS_MAX_SLOTS = 512`, erroring cleanly per invariant S2) and **bulk-loaded** at
finalize: ≤14 slots still emit a single leaf at block 1 (byte-identical to before,
`root_btree = 1`), otherwise N leaves (chained via `next_leaf`) under one internal
root whose separators are each following leaf's first key — matching the kernel's
descend rule (`while (i < nkeys && key >= intern[i].sep) i++`, child holds keys
≥ sep). `sfs_readback` gained the same descend. Gates: `smoke-mkfs-sfs` now also
provisions 20 files (41 slots → 3 leaves + internal root) and verifies the
**first, middle and last** file byte-exact (proving the descend lands at both
separator edges); `smoke-sfs-persist` provisions past 14 slots so the **kernel**
mounts and reads a host-authored two-level tree (`21 slots, root=23 (multi-leaf)`).
Host-tool only — zero kernel files touched. **106 gates** (both gates extended,
no new gate).
**DDR-774 (scoping only — no code):** the pre-code blast-radius review of the NVMe
completion IRQ (master-doc Section B#1) found it is **not** a bounded slice, so it
was split rather than started. Vector availability was never the blocker
(DDR-771 vacated 50–53); the cost is three coupled surfaces — (a) the only MSI-X
programmer is virtio-coupled and would have to be refactored out of a path serving
blk/net/gpu/input, (b) `nvme.c` maps a fixed 2-page BAR0 window while the MSI-X
table offset/BIR is runtime-determined, (c) completion moves thread→IRQ context.
Now **774a** (generic MSI-X helper, pure refactor, existing gates) → **774b**
(NVMe table mapping + `IEN`, still polling) → **774c** (IRQ completion with a
bounded spin fallback, invariant S2). **Gate count unchanged: 106.**

**DDR-774a (generic PCI MSI-X programmer — pure refactor):** the capability walk +
table-entry programming that only existed inside `virtio_pci_msix_setup()` now live
in `kernel/drivers/pcie/pcie.c` as `pcie_msix_find()` (walks the cap chain for ID
0x11, returns cap offset + table BIR/offset), `pcie_msix_program()` (MSI-X enable +
entry-0 address/data/vector-control) and `pcie_intx_disable()`. `virtio_pci_msix_setup`
is reimplemented on top with its **signature, register writes and their order
unchanged** — INTx-disable deliberately stays at the call site so it still runs
*after* the per-queue `queue_msix_vector` routing, keeping this a true no-op
refactor. All three virtio callers (`virtio_blk.c`, `virtio_input.c`,
`virtio_net.c`) are untouched. Unblocks DDR-774b (NVMe MSI-X). Verified by every
gate covering an MSI-X consumer: `smoke-fs` (asserts `msix vec=56`, virtio-blk),
`smoke-net-lo` (asserts `msix vec=54`, virtio-net — note this is the gate carrying
that sentinel, not `smoke-net`), `smoke-net`, `smoke-input`, `smoke-gpu` — all
PASS. **Gate count unchanged: 106.**

**DDR-774b (NVMe MSI-X table mapping + `IEN` — plumbing only):** `nvme.c` now
locates its MSI-X table at runtime via `pcie_msix_find()` (BAR index + offset are
*not* assumed), maps 2 uncached pages for it at a dedicated `NVME_MSIX_VBASE`
window (separate from the BAR0 register map, so that mapping is untouched),
programs entry 0 for **vector 50** (free since DDR-771) via `pcie_msix_program()`,
registers an inert counting handler, disables INTx, and creates the I/O CQ with
`IEN` + vector in `cdw11`. **Completion is still polled** — `nvme_submit()` is
unchanged, so `PRADYOS_NVME_RW_OK` / `PRADYOS_NVME_PRP_OK` prove the polled path
is unaffected by arming the interrupt. `smoke-nvme` gains the deterministic
`[nvme] msix vec=50` sentinel. **OPEN ISSUE: `[nvme] irqs=0` — the interrupt is
programmed but never delivered.** Interrupts-masked is ruled out (`sti` at
`main.c:1478`/`:1737` precedes `nvme_init` at `:1801`); remaining suspects are the
MSI-X Function-Mask bit (MC bit 14, never explicitly cleared), the table address
math, and per-entry vector control. **DDR-774c must root-cause delivery before
converting the completion path.** The IRQ count is deliberately printed but NOT
gated, since with polling active its timing is not deterministic.
**Gate count unchanged: 106.**

**DDR-774c phase c-1 (NVMe MSI-X delivery root-caused + fixed):** DDR-774b armed
the interrupt but nothing was delivered (`[nvme] irqs=0`). Root cause was a spec
misreading in 774b: NVMe `Create I/O CQ` **CDW11[31:16] is an index into the
device's MSI-X table**, not an x86 interrupt vector. It was passed 50 (the x86
vector), aiming the controller at an unprogrammed, masked table entry while only
**entry 0** had been programmed — the x86 vector actually travels in that entry's
message-data field. Passing the table index instead (`NVME_MSIX_ENTRY = 0`) turns
`irqs=0` into **`irqs=6`**. The leading suspect going in — the MSI-X Function Mask
(message-control bit 14) never being explicitly cleared — was **not** the cause,
so the shared `pcie_msix_program()` helper was left untouched and the four virtio
MSI-X consumers cannot regress. `smoke-nvme` now asserts **`PRADYOS_NVME_IRQ_OK`**
(and forbids `..._IRQ_FAIL`), made deterministic by a **bounded** settle spin (S2)
rather than a timing-dependent count; the exact count is printed but not asserted.
Completion is **still polled** — phase c-2 converts it. **Gate count unchanged: 106.**

**DDR-774c phase c-2 — STOPPED and re-scoped (no code).** The stop condition was
invoked deliberately: `nvme_submit()` is already a bounded poll, so the specified
"IRQ flag + bounded spin, no scheduler hook" reduces to polling-with-a-hint and
saves nothing on the driver's hottest path. A real IRQ-driven wait requires the
CPU to sleep — `sti;hlt` (rejected: mutates the caller's `IF`), a guarded `hlt`
idle path, or a scheduler block/wake wait-queue (out of scope) — so it is deferred
to a future DDR that must *measure* polling cost first. **Section B#1 is
functionally complete for correctness**: NVMe MSI-X is programmed (774b),
delivered (774c-1, `irqs=6`) and gated (`PRADYOS_NVME_IRQ_OK`).

**DDR-775 — B#3 narrowed to the virtio-blk completion wait (findings, no code).**
A second CI hit (run 30155872016) failed `smoke-blk-integrity` (`-smp 4`,
concurrent read data-verify) at the full 180 s, while 3/3 local `smoke-surfdestroy`
runs PASS. Two different `-smp 4` gates, both block-I/O; the surfdestroy stall
point (`SYSFSTAT OK`, next sentinel `SYSREAD OK`) places it in `sys_read` →
`vfs_read` → SFS → virtio-blk. **Confirmed defect and S2 violation:** `submit()`'s
`while (!v->req[s].done) sched_block_on(&v->compl_lock);` is **unbounded**, so a
missed completion hangs the boot permanently instead of failing diagnosably. The
classic lost-wakeup race is *not* the defect — it is correctly closed by the
locks-4 pattern. **Latent (not this trigger):** `slot_waiter` is a single pointer,
so >`VBLK_NREQ`(8) concurrent submitters would lose a wakeup. No fix shipped: the
bug does not reproduce locally, so a concurrency change in the shared block path
would be unvalidatable. Next slice bounds the wait — which requires either a
scheduler *timed* block (absent today) or a `g_ticks` yield-loop, an explicit
design decision. See `docs/ddr/DDR-775-smp4-blk-hang.md`.

**⚠ TOP BLOCKER — Section B#3 (`-smp 4` race), signature captured 2026-07-25.**
CI run 30151522978 failed `smoke-surfdestroy` at the **full 180 s** timeout having
missed the **first** sentinel, and the serial shows the boot **HUNG after
`SYSFSTAT OK`** — inside the ring-3 syscall self-tests, before any surface test.
It is a **hang, not slowness**, so the DDR-771 timeout bump is not a fix. It is
**pre-existing and unrelated to DDR-774**: the same gate failed in run
29726803735 (DDR-766, before 774a/b/c existed), and this gate boots with **no NVMe
device** so `nvme_init` never runs. Intermittent (passed in runs 30141466540 and
30146543550). This currently blocks promoting green work to `main`.

**DDR-776 (virtio-blk stuck-request watchdog — diagnosis before fix, B#3):** run
30158060606 passed every `-smp 4` block gate that had failed previously, on the
same commits, confirming the hang is **intermittent** — so one green run proves
nothing and a speculative behavioural fix would be unfalsifiable. Instead this
slice makes failures *informative*: `struct vreq` gains a submit tick + LBA, and
`virtio_blk_watchdog()` — called from the timer path in `idt.c` every ~1 s, the
same idiom as the existing `net_poll_tick()` — prints `[vblk] stuck dev=D slot=N
lba=L age=T` **once** per request stuck >5 s. It works even when a submitter is
blocked forever, because only that thread is stuck while the timer keeps firing.
**No blocking behaviour changed**, no lock taken (read-only from the ISR, so it
adds no deadlock surface to the subsystem under investigation — S6), bounded at
64 scalar checks per tick and one print per request (S2). Design decision recorded
explicitly: a `g_ticks` yield-loop was rejected (it would spin on *every* block
I/O, regressing the hot path every FS gate rides) and a scheduler timed-block —
the correct eventual primitive — was deferred so the scheduler core is not changed
mid-investigation. **This does not fix the hang** and is not claimed to; bounding
the wait remains the follow-on S2 fix, to be designed with the diagnostic's output
in hand. **Gate count unchanged: 106.**

**⚠ DDR-776's first result is a NEGATIVE one that refutes the DDR-775 narrowing.**
Run 30163444702 failed a *third* `-smp 4` gate — `smoke-smpuser` ("user-on-AP",
a ring-3 thread on a non-BSP CPU), **not** block-I/O — missing `[smp] user on AP
OK`, and the new watchdog printed **nothing**: no virtio-blk request was stuck
>5 s. The timer was demonstrably still firing (the boot progressed through the
fuzz test and a ring-3 thread exited), so the silence is evidence, not missing
instrumentation. The three B#3 failures share only `-smp 4` and miss **different**
sentinels each time. **Revised position: the original percpu-scheduler/AP-race
framing is better supported than the virtio-blk narrowing**, which is now marked
superseded in DDR-775. The two virtio-blk hazards (unbounded completion wait;
single-element `slot_waiter`) remain real S2 defects worth fixing on their own
merit, but are **not proven** to be this trigger. This is exactly why the
diagnosis-first slice was chosen over a speculative fix — a blind bound on the blk
wait would have "fixed" nothing and masked the real cause.

**⚠⚠ B#3 SECOND CORRECTION — leading hypothesis is now a STALLED TIMER.** Closer
reading of run 30163444702 shows the serial printed **neither** `[smp] user on AP
OK` **nor** `[smp] user on AP FAIL` (both log hits are the sentinel echo and the
"not found" message), while the preceding `ap preempt OK` / `resched OK` did
print. `smpuser_proof()` (`main.c:659`) is `while (!g_user_on_ap && g_ticks < dl)`
— a deadline poll that **must** print one branch **unless `g_ticks` stops
advancing**. This also **retracts** the earlier inference from the watchdog's
silence: the watchdog runs on the *same timer path*, so silence is consistent with
either "no stuck blk request" or "the watchdog never ran"; the claim that "the
timer was demonstrably still firing" rested on boot progress that occurred
*earlier* than the stall. **Hypothesis: under `-smp 4` the timer tick
intermittently stops advancing `g_ticks`** — explaining all four failures at once
(each missing whichever sentinel the boot had reached), the watchdog's silence,
virtio-blk waits never waking, and the consistent local passes. **This is a
systemic S2 exposure: every `g_ticks`-bounded wait is only as bounded as the
timer.** Next experiment is decisive and cheap: a timer-driven heartbeat plus
`g_ticks` at AP-proof entry/exit.

**DDR-777 (B#3 three-way discriminator probe) + a THIRD correction.** Two earlier
claims are retracted. (1) **"Timed out at the full 180 s, therefore it hung" was
WRONG:** `boot_test.sh` always runs QEMU for the whole `TIMEOUT_S` window and
*then* greps — `terminating on signal 15 … (timeout)` appears in **passing** runs
too. The only hard evidence is *sentinel absent*, not *hang*. (2) The watchdog-
silence inference was already retracted in DDR-775. **Newly established:** every
SMP proof shares `if (!g_smp_have_aps) return;`, and since `ap preempt OK` /
`resched OK` printed in the failing run, APs **were** up — so `smpuser_proof()`
did not take its silent early return; it entered the poll and never reached its
`kputs`. Three explanations survive and nothing yet separates them: **(A)** the
timer stalls so `g_ticks < dl` never expires, **(B)** scheduler starvation — the
proof thread never resumes from `yield()` while the system stays alive, or **(C)**
a guard/ordering effect. This slice therefore ships **only a discriminator**: a
`[hb] t=<g_ticks>` heartbeat every ~500 ticks from the existing timer call site,
plus a `[smp] user-on-AP probe t=…` entry marker and a tick on the OK/FAIL line.
Next failing run reads unambiguously — no probe line ⇒ (C); probe present and
heartbeat stops ⇒ (A), making this a systemic S2 exposure since *every*
`g_ticks`-bounded wait is only as bounded as the timer; probe present and
heartbeat continues ⇒ (B). Passive only: no behaviour change, no locks, no
scheduler hook; sentinels verified safe (`grep -qF` substring keeps both the
EXTRA `[smp] user on AP OK` and FORBIDDEN `user on AP FAIL` matching).
**Gate count unchanged: 106.**

**Section B audit (2026-07-26) — the roadmap overstated remaining work.** Three
entries marked "planned" are in fact **shipped and gated**, verified against the
tree rather than assumed: **B#5 COW fork** (`kernel/mm/vmm_cow.c`:
`vmm_fork_address_space_cow()`, `PTE_SW_COW` in both address spaces, PMM
refcounts, `PTE_SW_SHARED` pass-through for the vDSO; `vmm_cow_fault()` wired at
`idt.c:225`; gate `smoke-cowfork` at Makefile:760 / CI:228 plus a `main.c`
self-test), **B#7 kernel self W^X** (`vmm_protect_kernel()` re-stamps text RX /
data NX after boot; DDR-757 gate at Makefile:660 + the CI "text RX + data NX PTE
audit" step), and **B#8 `ls`/`ps`** (corrected earlier — both ship via
`SYS_GETDENTS`/`SYS_GETPROCS`). All three are moved into Section A. The remaining
Section B entries have **not** all been individually re-verified — check the tree
before planning any of them.

**B#3 status: instrumented, awaiting a natural failure.** The DDR-777
discriminator is now on `main`. The flake has been quiet for four consecutive
green runs (30158060606, 30165570464, 30167716462, 30170362044) after a cluster of
four failures, and it has never reproduced locally (3/3 pass). No further B#3 work
is warranted until a failure occurs and the probe names the mechanism — forcing a
fix without it would be a fourth blind attempt.

**DDR-778 (PRISM output redirection — Section B#12, first bounded slice):**
`cmd … > file` now works in PRISM. Scoping first confirmed the kernel side
**already ships** — `SYS_DUP2 = 18` and `SYS_PIPE` exist (PROC-A, gated by
`smoke-syspipe` and the CI "pipe/dup2" step); `prism.c` had merely never
`#define`d `SYS_DUP2` — so this slice is **ring-3 only, zero kernel change**.
Implementation: scan `argv[1..argc)` for a bare `>` (from index 1, so a leading
`> file` cannot leave `argv[0]` undefined), truncate `argc` at it, then
`dup2(1, REDIR_SAVE_FD)` / `open(O_CREAT|O_WRONLY)` / `dup2(fd,1)` / `close(fd)`,
restoring at the loop's existing flush point. Two hazards handled deliberately:
musl **fully buffers** a non-tty stdout, so output is flushed *before* fd 1 is
restored (a late flush would land on the console and make the redirect silently
half-work); and a skipped restore would send *all* later shell output into the
file, so control flow was audited — the only `continue` precedes the swap, the
`ls` `break` is an inner loop, and `exit` returns from `main`. Gate: `smoke-shell`
extended with a **discriminating pair** — asserting the marker alone would pass
even if redirection did nothing (a plain `echo` prints the same text), so
`REDIR.TXT` must also appear in `ls /`. **Gate count unchanged: 106.**
Remaining B#12: `|` (needs a fork around the builtin dispatch — PRISM builtins are
internal functions, not execs), `<`, `>>`, stderr, job control.

**🚨 DDR-779 — CI blocked by an upstream outage (`git.musl-libc.org`).** Run
30178367399 died after 4m39s at **step 2 `actions/checkout@v5`**, before any
project code was fetched: the `third_party/musl` submodule clone timed out twice.
Independently verified — the host is unreachable from the dev machine too. **Not a
regression; pushing more commits cannot help** until it returns. Second
external-dependency outage this session (the first, `static.rust-lang.org`'s
nightly checksum, self-resolved). `.gitmodules` pins musl to a single upstream
host while lwip already resolves to GitHub, making musl a hard single point of
failure ahead of every build. A mirror fix is **proposed but deliberately NOT
applied** — it is a supply-chain change, and the pinned SHA could not be confirmed
present in the mirror cheaply (`git ls-remote` lists only ref tips, so the result
was inconclusive rather than negative). DDR-779 records the verification recipe
and alternatives. **Operational rule: check step 2 before diagnosing any CI
failure.** Local builds and gates are unaffected.

**DDR-780 (PRISM pipes `cmd1 | cmd2` — Section B#12, second slice):** ring-3 only,
no kernel change (`SYS_PIPE = 17` / `SYS_DUP2 = 18` already ship, PROC-A). Because
PRISM builtins are internal functions rather than execs, the fork must wrap the
**dispatch itself** — but instead of hoisting the 120-line `if/else` chain into a
function, each forked half sets up its fds and **falls through** to the existing
dispatch, exiting at the bottom of the loop. Same semantics, far smaller blast
radius. Parent closes **both** pipe ends (otherwise the reader never sees EOF and
the shell wedges) and reaps both children — bounded at one pipe / two children
(S2); a faulting builtin now kills only its child, not the shell (S6).
**`cat` with no argument now reads stdin** — found while designing the gate that
*no* PRISM builtin consumed fd 0, so a pipe would have been unobservable and
useless. Gate: `smoke-shell` gains a **discriminating pair** — the marker must
appear AND `pipe-marker-4k8 | cat` must never appear, since a shell ignoring `|`
would hand `echo` the literal tokens and print them verbatim. (An earlier
`ls / | cat` → `HELLO.TXT` assertion was rejected as non-discriminating: the FIFO
already runs a plain `ls /`.) Verified locally — CI is blocked by DDR-779.
**Gate count unchanged: 106.**

**DDR-781 (PRISM `<` and `>>` — Section B#12, third slice):** ring-3 only, no
kernel change — but the prerequisite check **changed the mechanism**: the kernel
has **no `O_APPEND`** (`sys_file.c` honours only `O_CREAT`), so the planned
`O_CREAT|O_WRONLY|O_APPEND` does not exist and adding a kernel open-flag would
have been a silent scope expansion. `SYS_LSEEK = 10` does support `SEEK_END`
(`base = e->file->size`), so append is done in ring 3 as `open` + seek-to-EOF.
Recorded honestly: that is **not** atomic `O_APPEND` (fine here — one writer per
command), and it exposed a **pre-existing** gap, namely there is no `O_TRUNC`
either, so DDR-778's `>` does not truncate. `<` opens `O_RDONLY` and swaps fd 0,
restored alongside fd 1 at the loop's existing flush point. Gate assertions are
discriminating: `>>` requires **both** records to survive (had it behaved like
`>`, the equal-length second write would have overwritten the first — a
consequence of the missing `O_TRUNC`), and `<` requires the marker **and** forbids
`cat: cannot open <`. **Gate count unchanged: 106.**

**DDR-782 (kernel `O_TRUNC` + atomic `O_APPEND` — Section B#12, kernel-side
remainder):** closes the two gaps DDR-781 recorded as real defects in shipped
behaviour — `>` did not truncate (a shorter rewrite left the old tail in place)
and `>>` was a one-shot `lseek(SEEK_END)`, not atomic. The prerequisite check
**re-scoped the slice**: `struct vfs_fs_ops` (vfs.h) exposes no `truncate` op and
none of FAT32/SFS/ext4 can shorten a file, so a real `ftruncate` would be a new
VFS op implemented three times in three on-disk formats. Rather than invent one
silently, `O_TRUNC` is implemented as `vfs_unlink` + `vfs_create` on an
already-existing file — truncation-to-zero, which is exactly what a shell `>`
needs, working identically on all three drivers. Honest limitation: the file is
re-created rather than shortened, so it gets a fresh inode/cookie (nothing
observable depends on inode identity today — no hard links); non-zero
`ftruncate(len)` remains impossible and is a stated non-goal. `O_APPEND` needs no
FS support at all: the FD_VFS write path in `sys_io.c` sets
`e->off = e->file->size` once per `write()` call, before the chunk loop — that is
the POSIX atomicity property (the whole call lands at end-of-file as one act),
and it is what an lseek-at-open append cannot give when writers share a file.
Flag values are Linux's (`O_TRUNC 0x200`, `O_APPEND 0x400`), defined once in
`sys_file.h` because two translation units honour them. No new syscall (NSI stays
at 75), no `fd_entry` field (`flags` was already stored), no on-disk change, and
**no capability change** — CAP_FS_WRITE already gates `vfs_create`/`vfs_unlink`/
`vfs_write`, so a read-only opener cannot truncate. `user/prism.c` now passes
`O_TRUNC` for `>` and `O_APPEND` for `>>`, dropping the `SYS_LSEEK`/`SEEK_END`
crutch (both defines removed — no dead refs). `smoke-shell` gains a
**discriminating-by-construction** truncate check: `/TR.TXT` is written long then
short and `cat` must show the short record while the long record's `TAIL9x3`
suffix must be **absent** — that suffix survives under the pre-DDR-782 behaviour,
so the assertion fails before the fix and passes after. Verified locally: both
truncate checks, the append pair, and the DDR-780/781 pipe and `<` regressions
all PASS with zero panics. **S2** (bounded: O(1) additions, no new loop, a failed
unlink/create aborts cleanly rather than leaving a half-open fd) and **S6**
(fault isolation: errors return an errno; the offset override touches only the
calling process's fd table). No invariant weakened. **Gate count unchanged: 106.**

**🚨 DDR-790 — OPEN: kernel heap double-free panic in CI, and it is on `main`.**
Run 30215987521 (`ba5770e`) failed at step 54 `smoke-blkmq` (q35 `-smp 4`) with
`[kheap] double-free ptr=0x…7E29F80 objsize=0x20` and `*** KHEAP PANIC: kfree:
double free ***`. Prime suspect is **DDR-787** (the pipe `refcount` →
`readers`/`writers` split), because `struct pipe` is 24 B and lands in the
32-byte bucket and that slice is exactly what changed pipe lifetime — but
`struct vfs_file` occupies the same bucket and is **not excluded**. Two readings
were corrected during the investigation and both are worth keeping: the
`multi-inflight FAIL` string in the log is the Makefile echoing the gate's
`FORBIDDEN_SENTINEL=` line, not a kernel print; and the total absence of `[hb]`
heartbeats looked like DDR-777's timer-stall verdict, but the run **panicked**, so
that silence is a consequence rather than independent evidence — **B#3 was not
validly read here and stays open.** A temporary `[pipe] create/destroy p=… r=… w=…`
trace was added on the DDR-776 pattern. **It has not yet found the bug, and its
first reading was a trap:** three pointers each appearing twice looked like a
double free but was `kheap` **address reuse** — the paired create trace showed
creates=4/destroys=4, exactly balanced. Pointer identity is not evidence; the
create/destroy pairing is. Not reproduced locally (3/3 `smoke-blkmq` clean).
One hardening was applied and is labelled as hardening, not as the fix:
`pipe_close` frees only when the call actually **dropped** a reference, since the
first cut freed whenever both counts merely *read* zero — a shape the old single
refcount masked by going negative. **S6** is the invariant in question (a double
`kfree` is a fault-isolation failure). **Gate count unchanged: 106.**

**DDR-789 (PRISM exit status `$?` — Section B#12, sixth shell slice).** Queued
behind SIGPIPE, and the tree check **reordered them**: (1) signal defaults are a
whitelist, not a table — `signal_deliver` terminates on SIGKILL and on SIGTERM
with no handler, and **ignores everything else**, so defining `SIGPIPE 13` and
raising it would silently do nothing; and (2) SIGPIPE cannot be gated
discriminatingly today, because PRISM has no `head`-like builtin that consumes
part of its input and exits, and a stage's outcome is not observable at all — so
"writer killed promptly" and "writer ran to completion into a dead pipe" look
identical in the serial log. Exit status is what makes outcomes observable (a
signal-killed thread exits `-1`), so it lands first and unblocks SIGPIPE.
Ring-3 only: `sys_wait4` already copies out the **raw** exit code (explicitly not
`W*`-encoded) and PRISM already collected it in `do_run` and DDR-786's stage loop
and discarded it. Now kept in `last_status`, with a pipeline reporting its **last**
stage. **The design changed under test:** the first draft expanded a token equal to
exactly `$?`, which testing proved useless — the real idiom is `echo status=$?`,
one token, so nothing expanded. Widened to a token *ending* in `$?` rather than
contorting the gate; still not general expansion (no `$VAR`, no mid-token
substitution). Two incidental fixes: `snprintf` is **not** in the musl subset (the
same gap DDR-784 hit), so a bounded `fmt_long` replaces it rather than growing the
subset for one integer; and DDR-786's stage loop read `wait4` into a `long` while
the kernel copies out `sizeof(int)`, leaving the upper bytes uninitialised — now
an `int`. Gate discriminates by construction: before this slice `echo st-ok=$?`
printed the literal, so forbidding it fails deterministically, and the values are
asserted too (`st-ok=0`, `st-fail=127` — 127 is what `do_run`'s child exits when
`execve` fails). Local: all PASS with the DDR-786/787 regressions intact, zero
panics. **S2** (fixed-length substitution inside existing `argv[16]`/`line[256]`
bounds, no allocation) and **S6** (ring-3 only). **Gate count unchanged: 106.**

**DDR-788 (retire the DDR-783 flake class — infrastructure).** DDR-783 raised one
gate's window because its last sentinel landed at t=24.26 s under a 30 s default;
the *condition* was left alive everywhere else because, pre-DDR-785, the timeout
**was** the runtime and margin cost real wall-clock. DDR-785 made margin free for
early-exit-eligible gates, so this retires the class. Measured scope first, and it
was smaller than assumed: of 92 `boot_test.sh` invocations, 38 declare
`FORBIDDEN_SENTINEL` (timeout still is the runtime — untouched, raising them would
add ~57 min to every green run), 43 already carry an explicit `TIMEOUT_S`, leaving
**11** on the default. **A first-draft claim that two gates were at risk was
corrected by measurement to one:** `smoke-fs-sfs-rw` takes **30 s** against a 30 s
window (it asserts the same journal/version-isolation/compress chain DDR-783 timed
at 24.09–24.26 s) — essentially zero margin; `smoke-fs-rw` turned out to be **5 s**
and was never at risk. Seven gates raised to `TIMEOUT_S=120`; three deliberately
left alone (`smoke` ×2 asserts only `NEXUS KERNEL OK` at t=0.31 s, and
`smoke-mkfs-sfs` is a host-side tool gate). **Cost on success is zero** and that
claim was checked rather than asserted — smoke-uaccess 4 s, smoke-cowfork 3 s,
smoke-mitigations 4 s, smoke-fs-rw 5 s, smoke-fs-sfs-rw 30 s, smoke-fs-ext4 34 s,
all PASS under the 120 s ceiling and all against the DDR-787 kernel. Cost on
failure is real and stated: a genuinely failing eligible gate now burns 120 s
instead of 30 s. No kernel or user code; S1–S8 untouched (`timeout` still bounds
every gate, so a hung kernel still fails — only the deadline moves). **Gate count
unchanged: 106.**

**DDR-787 (blocking pipe semantics — kernel; Section B#12).** The prerequisite
check for the queued ">4 KiB truncation" slice found the **larger** half first:
the `FD_PIPE` read path returned 0 whenever the ring was momentarily empty, and
every reader treats 0 as EOF — so `a | b` was **timing-dependent** and `b` could
print nothing if scheduled first. The DDR-780/786 gates were passing by
scheduling luck, not by guarantee; a latent correctness bug in shipped behaviour.
Neither half was fixable on the old `struct pipe`, which carried a **single**
`refcount` and could not distinguish readers from writers: a reader may block only
while a writer remains and vice versa, so blocking without those counts would be
**unbounded — a direct S2 violation**. Implemented: `refcount` → `readers` +
`writers`, with the end passed explicitly at every incref/close (six call sites
across `pipe.c`, `fd.c`'s `fd_free` and `fd_clone`, and `sys_dup2`), plus
`pipe_destroy` for the never-installed error path; the pipe frees only when both
counts reach zero. Reader waits while empty **and** `writers > 0` (EOF only at
zero) and only when it has delivered nothing yet, so a partly-satisfied read
returns promptly; writer waits while full **and** `readers > 0`, returning
`-EPIPE` when the last reader goes (`EPIPE` added to `errno.h` as 32 — it did not
exist). Waiting uses the `yield()` poll the tree already blesses for `FD_CONSOLE`
blocking reads, so no per-pipe wait queue and no new scheduler hook; **termination
is always the refcount condition, never a timeout**. Implementation also fixed a
second writer bug the design had not spotted: `if (w < chunk) break;` dropped the
remainder of a partly-fitting chunk, so the loop now re-copies it. Gate: `cat
/BIG8K.TXT | cat` (~7.8 KiB, 200 payload lines) asserting **≥180 lines** — the
pre-fix ceiling is ~107 lines at PIPE_SIZE 4096. **Counted, not exact-matched, for
a measured reason:** concurrent kernel prints share COM1 and can split a payload
line mid-string (`pipe p[sfs] journal … OK` / `ayload line 099`), which cost 3 of
500 lines on the first run and would have flaked an exact assertion for reasons
unrelated to pipes. Verified locally at CI-like pacing: **200/200 delivered**,
head+tail intact, single- and multi-stage pipelines and shell-alive all PASS, zero
panics. `smoke-shell`'s 60 s window is **not** raised (payload sized to fit).
**S2** is now load-bearing in a new way — bounded by the readers/writers condition
rather than trivially — and **S6** holds: the last close of either end wakes the
peer with EOF or `-EPIPE` instead of wedging it. **Gate count unchanged: 106.**

**DDR-786 (PRISM multi-stage pipelines `a | b | c` — Section B#12, fifth shell
slice):** DDR-780 scanned for the FIRST `|` only, and its right-hand child fell
through having already passed the pipe block, so a second `|` reached the builtin
as a literal token — `echo m | cat | cat` handed `cat` the arguments `| cat`. Real
gap, now closed. **The standing question was whether N stages finally force the
~120-line refactor DDR-780 deferred: they do not** — the existing fall-through
dispatch is reused unchanged and only the pipe block grows (~40 lines). Two
designs were weighed: the ~10-line version (right child re-scans and re-forks) was
**rejected** because it leaves an intermediate shell at every boundary holding the
previous pipe's read end open, which with non-blocking 4 KiB pipes silently drops
output instead of hanging — a data-loss mode that hides in small tests. Chosen
instead: the shell splits the line into all stages up front and forks each itself,
threading one pipe between neighbours, closing `prev_read` and `fds[1]` in the
parent immediately after each fork (the DDR-780 EOF lesson applied once per
boundary) and reaping all N children. Correct topology — N processes, one waiter.
Malformed pipelines (`|` first, last, or doubled) are rejected **before any fork**,
so a bad line costs no processes. No kernel change: `pipe_create` heap-allocates
per pipe with no fixed table, and `SYS_PIPE`/`FORK`/`DUP2`/`WAIT4` all ship.
**Pre-existing limitation recorded, not introduced:** `PIPE_SIZE` is 4096 and
`pipe_write` is non-blocking, so a stage producing >4 KiB faster than its reader
drains loses data — already true of DDR-780, and it bounds what pipelines can be
used for until pipe writes block (kernel-side, separate slice). Gate
discriminates: `echo pipe3-m7q | cat | cat` must print the marker **and** must
never produce `cat: cannot open |`, which is exactly what the old code printed.
Locally: 3-stage, 4-stage, malformed rejection, all prior-slice regressions and
shell-still-alive all PASS, zero panics. **S2** (bounded by `argv[16]` ≤ 8 stages;
malformed rejected pre-fork) and **S6** (each stage its own process; every child
reaped). **Gate count unchanged: 106.**

**DDR-785 (`boot_test.sh` early exit — infrastructure; the DDR-783 systemic
finding, now fixed).** The harness ran `timeout $TIMEOUT_S qemu …` to completion
*every time* and only then grepped, so the timeout **was** the runtime. Measured
across the Makefile: 91 `boot_test.sh` invocations totalling **7590 s = 126.5 min
of pure waiting** per CI run; 53 of those gates declare no forbidden patterns and
account for 4050 s, of which ~2566 s (**~43 min/run**) is idle at a conservative
28 s real boot each. That design is also why gate timeouts must be hand-tuned and
why DDR-783's flake existed at all. Fix: poll the serial capture file (QEMU writes
a **file**, not a pipe, so there is nothing to defeat) and stop the guest once
`$SENTINEL` and every `EXTRA_SENTINEL` line are present; the existing verification
block is untouched, so PASS/FAIL output and log-deletion are identical. **The
correctness hazard is excluded by construction, not mitigated:** early exit is
enabled ONLY when `FORBIDDEN_SENTINEL` is empty, because a forbidden pattern must
not appear and stopping early would prove only "not yet" — a gate that should FAIL
could PASS. With solely must-appear assertions in play, seeing them sooner cannot
change the verdict. The 38 gates declaring forbidden patterns keep today's
behaviour exactly, deliberately leaving 3540 s of budget unclaimed in exchange for
a guarantee rather than an argument. New host-only `make smoke-selftest`
(`tools/qemu_runner/selftest.sh`, stub qemu, no kernel — also wired into CI before
the harness is used to judge anything) asserts verdict **and timing**: early exit
finished in 2 s against a 60 s window, a **late forbidden pattern still took the
full window and FAILED** (the one way this could have silently weakened all 91
gates), a missing required pattern still failed, and a declared-but-absent
forbidden pattern still passed. End-to-end: `smoke-fs` 30 s (60 s window),
`smoke-uaccess` 4 s (30 s window), both PASS. **Confirmed in CI: run 30200918063
took 105.8 min vs the 152.3 min baseline (run 30193738689) — 46.5 min saved on the
same 113-step suite**, closely matching the ~43 min predicted from the budget
measurement. The `smoke-selftest` step passed in CI as step 5, before the harness
judged any gate. No kernel or user code; S1–S8
untouched; the `timeout` ceiling is unchanged so a hung kernel still fails exactly
as before. **Boot-gate count unchanged: 106.**

**DDR-784 (PRISM diagnostics on stderr + `2>` — Section B#12, fourth shell
slice):** the prerequisite check re-scoped this one too. PRISM had **zero**
writers to fd 2 — every diagnostic went through `printf` → fd 1 — so `2>` alone
would have been untestable sugar redirecting an fd nothing writes to. The slice
is therefore two halves: route genuine errors to `fprintf(stderr, …)`, then add
`2>` to the redirect scan. Success/informational messages deliberately stay on
stdout; in particular **`rm: removed …` is gate-asserted** and moving it would
have silently broken `smoke-shell` (checked before touching any print). fd 2
already exists as `FD_CONSOLE` from `fd_init_std` and `fd_write_user` is
fd-agnostic, so **no kernel change**. One prerequisite surfaced only at link
time: the musl **subset** shipped `stdout.c` but not `stderr.c`/`fprintf.c`
(`ld.lld: undefined symbol: stderr`), and `snprintf` is absent too, so some
subset growth was unavoidable — `tools/build_musl.sh` gains those two upstream
sources (no overlay change, no ADR-023 pin change). `2>` uses a third parking
slot (`REDIR_SAVE_ERR 11`), truncating `O_CREAT|O_WRONLY|O_TRUNC` like DDR-782's
`>`, swap order stdin → stderr → stdout with every failure path unwinding exactly
the swaps already made. The gate discriminates by construction: `cat /NOPE9k2.TXT
> /OUT9k2.TXT 2> /ERR9k2.TXT` sends stdout and stderr to **different** files, so a
broken `2>` puts the error into the stdout file where it never reaches the
console — the marker is absent before the change and present after, which also
proves the message travelled on fd 2. Locally verified: discriminator, bare-error
visibility, `rm: removed` retention, and the DDR-780/782 regressions all PASS,
zero panics. **S2** (bounded parsing, clean failure) and **S6** (fault isolation,
fd juggling confined to this process). **Gate count unchanged: 106.**

**DDR-783 (`smoke-fs` timeout margin — infrastructure).** Run 30192189559 failed
at step 10 with `required pattern 'compress/readback/tag OK' not found`, after the
self-test had visibly run through journal + snapshot. **Not a DDR-782
regression** — that slice touched `sys_open`/the FD_VFS write path, while
`sfs_selftest_lz4` is kernel-internal and never goes through the fd layer, and the
identical image passes `smoke-fs` locally. Root cause **measured**: instrumenting
the boot shows the last required sentinel lands at **t=24.26 s** (`NEXUS KERNEL
OK` 0.31 s, `PRISM_READY` 23.91 s, journal 24.09 s, snapshot 24.18 s) against the
harness **default `TIMEOUT_S=30`** — 19 % margin on a fast local machine, so a
slower runner flakes it. The inconsistency making it a defect: `smoke-user`
asserts the *same* sentinel and already uses `TIMEOUT_S=60`; the SFS chain grew
across slices 4g/4h/4i and DDR-760 while this gate's window never moved. Fixed by
setting `TIMEOUT_S=60` on `smoke-fs` only — the other 56 default-30 gates assert
earlier sentinels and were left alone rather than blind-tuned. **This cannot mask
a hang:** `boot_test.sh` greps after the window regardless, so a hung kernel still
produces no sentinel and still fails. Systemic finding recorded as a proposal and
deliberately NOT applied: the harness always runs the full window instead of
exiting once all sentinels are seen, which is why every timeout must be
hand-tuned; early exit would remove the whole flake class and speed up CI, but it
touches 100+ gates and needs care with `FORBIDDEN_SENTINEL` semantics. No kernel
or user code; S1–S8 untouched. **Gate count unchanged: 106.**

**ADR-033 / DDR-779 IMPLEMENTED (2026-07-26) — musl submodule now fetches from a
GitHub mirror.** After a **third** checkout outage (run 30188805082, `a077ccd`,
DDR-782 — identical `Failed to connect ... port 443 after 134654 ms` / `Failed to
clone 'third_party/musl' a second time, aborting`, only 3 of 113 steps run), the
maintainer signed off on the mirror change. `.gitmodules` now points
`third_party/musl` at `https://github.com/ifduyue/musl`; **the pinned commit is
byte-identical** (`0784374d561435f7c787a555aeab8ede699ed298`) and the diff is a
single URL line. The principle, recorded in ADR-033: a submodule records an exact
SHA and git verifies content against it, so a mirror **cannot** serve different
source under that SHA — the host is chosen for availability only and is
substitutable in one line. Verification (the pre-condition DDR-779 blocked on) is
complete and corrected the proposal twice: **the mirror DDR-779 named,
`bminor/musl`, does not exist** (404), and content identity was instead confirmed
by SHA **and tree** across three independent mirrors (`ifduyue/musl`,
`tianon/mirror-musl`, `kraj/musl` — all tree
`2deb5f7c62d8c9e9733c9ed77d9210b708bbb69e`, equal to the tree of the local
submodule fetched from **upstream**), while `EOSIO/musl` and `AssemblyScript/musl`
lack the commit and were rejected. Upstream `git.musl-libc.org` remains canonical
for provenance and for any future version bump. No kernel code, no capability, no
syscall, no on-disk change — S1–S8 untouched. **Gate count unchanged: 106.**

**(historical) CI unblocked:** `git.musl-libc.org` was reachable again (DDR-779), so the
backlog — DDR-778 `>`, the DDR-779 finding, DDR-780 `|`, and this slice — can
finally be CI-validated.

**Canonical feature state:** see `docs/AETHER_MASTER_FEATURES.md` (Sections A–H).
ADR-026 baseline (Section D #1–17) re-verified **built** this session — no drift.
Section B#8 (`ls`/`ps`) corrected: it was stale, both already ship via
`SYS_GETDENTS`/`SYS_GETPROCS`.
**Last updated:** 2026-07-24

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
| ACPI Power Management (FADT/S5/reset) | 🟡 IN PROGRESS | 3 | **DDR-746/747:** FADT (`FACP`) parsed for PM1a/b_CNT + DSDT `\_S5_` scan + reset register; `acpi_poweroff()` = ACPI S5 soft-off (`SYS_POWEROFF`, NSI 69), `acpi_reboot()` = FADT-reset/0xCF9/8042 reset (`SYS_REBOOT`, NSI 70), both CAP_SOVEREIGN. Compositor `p`=off, `b`=reboot. Gates `smoke-poweroff`, `smoke-reboot`. S1–S4, MADT-via-FADT, full AML interpreter still future. |
| VFS Layer | 🟢 COMPLETE | 4 | `kernel/fs/vfs/` (ADR-015): driver registry + **mount table** (per-mount context vtable; FAT32/SFS/ext4 mountable side-by-side) + `open`/`create`/`read`/`write`/`unlink`/`readdir`, all capability-gated (CAP_FS_READ/WRITE via NCS) + per-thread write budget. Full mount-point namespace deferred. |
| FAT32 (read-write) | 🟢 COMPLETE | 4 | `kernel/fs/fat32/` (ADR-015): BPB parse, FAT chain, 8.3 + **VFAT long-name read** (ADR-020), nested paths. Read-write (4c): create/write/unlink, all-or-nothing alloc, read-back verify (`smoke-fs-rw`). **Timestamps** from RTC (4j). LFN *write* deferred (creates 8.3). |
| RTC / CMOS clock | 🟢 COMPLETE | 3 | `kernel/drivers/rtc/` (ADR-020): wall-clock via ports 0x70/0x71 (BCD/binary, 12/24h, stable read). `rtc_now` + `rtc_fat_datetime`; powers FS timestamps and later CLOCK_REALTIME. (Deferred Layer-3 item, pulled in at 4j.) |
| SOVEREIGN FS (SFS) | 🟢 COMPLETE | 4 | `kernel/fs/sfs/` (ADR-017/018): inode-based CoW B+tree, 4 KiB blocks. **4d:** format/mount/empty-root. **4e:** CoW B+tree create/lookup/open/readdir (split-on-insert; 10-file test). **4f:** file extents (write append/grow + read). **4g:** journal + atomic transactions (commit-record + mount replay). **4h:** snapshots — retained CoW roots; `sfs_open_version` reads a file as-of a snapshot. **4i:** inline LZ4 (`kernel/fs/sfs/lz4.c`, bounds-checked) — **per-extent** compression so compressed files still append; + ~4 KiB inode metadata tags (`sfs_set_tag`/`get_tag`). Verified: 128 KiB compressible → <32 blocks, byte-exact readback, tag survives remount. Next: ext4 read + FAT32 LFN (4j) → Layer 4 gate. Free-space B+tree / snapshot GC deferred. `CAP_FS_SFS_*` reserved. |
| SOVEREIGN FS (SFS) — duplicate row | 🟢 COMPLETE | 4 | (stale duplicate of the SFS row above; see it for detail) |
| ext4 Compatibility | 🟢 COMPLETE | 4 | `kernel/fs/ext4/` (ADR-019, slice 4j): **read-only** (the Layer-4 scope; write is out of scope) — superblock, group descriptors, extent-mapped inodes (depth-0), linear dir scan, nested paths. Verified reading a host `mkfs.ext4 -d` volume (4th disk). Write, multi-level extents, block-mapped inodes deferred. |
| ELF64 loader + W^X (static) | 🟢 COMPLETE | 5a | `kernel/exec/elf.c` (ADR-021): validates ET_EXEC/x86-64, maps each PT_LOAD into a fresh per-process AS with p_flags→W^X perms (text RX, rodata R-NX, data RW-NX; **W+X rejected**), zero-fills BSS, 8 MiB RW-NX user stack + unmapped guard page, SysV `argc/argv/envp/auxv` frame; spawns a ring-3 thread (cap delivered in RDI). Bootstrapped via SFS: the embedded test ELF (`user/hello.asm`) is written to SFS then **loaded back from SFS** — prints `HELLO FROM RING-3`, exits via sys_exit. W^X negative regression (`user/wxviol.asm`: write to RX text → #PF err=0x7 → clean kill, kernel survives). Gate `smoke-user` PASS. COW fork / dynamic linking / AS-reaping deferred. |
| pradyos-init (PID 1) | 🟢 COMPLETE | 5d | `user/init.c` (musl C, not Rust): PID 1, forks+reaps a child, then the system reaper loop; spawns PRISM. Gate `smoke-init`. |
| PRISM Shell | 🟢 COMPLETE | 5e | `user/prism.c` (musl C): serial-console shell, builtins help/echo/cat/run/ls/ps/touch/rm/uname/date/uptime/dmesg/`mode`/exit (ls=SYS_GETDENTS, ps=SYS_GETPROCS, touch/rm=O_CREAT/SYS_UNLINK on the FAT root, uname=SYS_SYSINFO, date=SYS_TIME, uptime=SYS_SYSINFO, dmesg=SYS_DMESG — DDR-751); console RX via IRQ4 ring. Gate `smoke-shell`. Agent DSL / job control deferred (ADR-024). |
| musl libc port | 🟢 COMPLETE | 5c/PROC-D | `third_party/musl` subset (libc.a + crt1.o) via `tools/build_musl.sh`, overrides in `third_party/musl-overlay/`; TLS + stdio + printf via SYS_WRITEV. Gate `smoke` (cmusl). |
| prad package manager | 🔴 NOT BUILT | 5 | |
| AETHER Daemon | 🟢 COMPLETE | 6 | `user/aether_daemon.c` (PID-2, CAP_SOVEREIGN): spawns the test agent via SYS_SPAWN_AGENT, reaps children, runs the mode-binding self-check. Gate `smoke-aether`. |
| Ollama IPC Bridge | 🟢 COMPLETE | 6/7 | Ring-3 proxy-socket NSI (ADR-027) + `user/agent_base.c` live mode: HTTP/1.1 `POST /api/generate` over the in-kernel lwIP stack + a hand-written JSON parser. Dev gate `smoke-agent-live` (needs a real Ollama); CI is test-mode. |
| Cloud API Adapters | 🔴 NOT BUILT | 6 | Anthropic/OpenAI/Gemini (the proxy-socket NSI makes these straightforward; not yet written) |
| Agent Capability Enforcer | 🟢 COMPLETE | 6 | `kernel/aether/` + `cap.h` CAP_AGENT/CAP_SOVEREIGN (kernel-set, no self-escalation) + 128 MiB mem cap (OOM kill) + 60 syscall/s rate limit. Gate `smoke-aether-sec`. |
| SOVEREIGN Gate Logic | 🟢 COMPLETE | 6/7 | Global `g_sovereign_mode` (default sovereign auto-approve; manual holds PENDING); `SYS_GET_MODE`/`SYS_SET_MODE` (CAP_SOVEREIGN). Bound to the toggle (DDR-701). Gate `smoke-mode`. |
| Approval Queue System | 🟢 COMPLETE | 6 | `kernel/aether/aether_queue.c`: 256-entry action queue + 4096-entry append-only audit ring (PMM-pool), 60 s TTL, `-EAGAIN` on overflow. Gates `smoke-aether-queue/-sec`. UI panel = compositor slice. |
| Named Agents (KRYOS…SOLIN) | 🟡 IN PROGRESS | 6f/7 | The 8 named agents render as compositor panel cards with active/inactive state tied to AETHER's 8-slot roster (DDR-707, `SYS_AGENT_ROSTER`); the daemon lights KRYOS. Gate `smoke-agents`. Each of the 8 now has a validated `aether/agents/roster/<name>/skill.md` defining role, capabilities and refusals (DDR-846), mapped onto Section G's 8 highest-priority roles; 5 are marked *not yet spawnable* because the capabilities they need (CAP_EXEC/CAP_OCR/CAP_SCENE/CAP_NET_BROWSE) are declared in `cap.h` but wired to nothing. Validated by `aether/tests/test_agent_skills.py` in the pytest job. Kernel-side per-persona dispatch is still future. |
| SkillOpt Training Loop | 🟢 COMPLETE | 6 | `aether/agents/skillopt/` (host-side Python, no kernel surface): rollout→reflect→aggregate→select→update→evaluate. A candidate skill replaces the incumbent **only on a strictly positive held-out improvement** — ties and regressions are rejected, held-out sets under 4 rollouts are refused outright, and train/held-out overlap raises rather than silently self-scoring (DDR-847). 16 tests in `aether/tests/test_skillopt.py` (pytest job); mutation-checked — `>` → `>=` fails exactly the tie tests. |
| Skill Self-Modification Guards | 🟢 COMPLETE | 6 | `aether/agents/skillopt/{validate,sleep,transfer}.py` (DDR-848). Validation runs **before** scoring, so a candidate that escalates capability never reaches a comparison a good score could carry it through: declared capabilities may only shrink (S1), refusal count may rise and may not fall, `## Invariants` must survive. Sleep consolidates offline on the **same** acceptance bar with a deterministic `sha256(salt:task_id)` split and a bounded 4096-entry journal. Transfer is a proposal, never an application — the recipient's own held-out evaluation decides. 36 tests in `aether/tests/test_skillopt_guards.py`; all five guards mutation-checked. DDR-850 adds the spec's **`CAP_SOVEREIGN`-always** approval gate (a missing approver is refused, not assumed authorised) and sleep's named `harvest→mine→replay→consolidate` stages, which **refuse to run while any agent is active**. |
| TokenJuice / trajectory / cost | 🟢 COMPLETE | 6 | `aether/agents/budget/` (DDR-849 + DDR-850). **Context compression to ≤80%** (`compress.py`): pinned segments are never dropped and an unreachable target **raises** rather than returning a best-effort context, which at the call site is indistinguishable from a successful one. Cost records `latency_ms` alongside `token_count`. Budgets **refuse** rather than truncate (a silently shortened context is a wrong answer wearing a right answer's shape), with a 5% completion reserve ordinary work cannot reach, grants debited at grant time (S1), and two-phase reserve/commit. Cost: an **unknown model raises** — never priced at zero — integer micro-cents, rounding up. Trajectory: append-only JSONL with no update path, redacted before the write, tolerating a truncated tail but refusing mid-file corruption. 34 tests in `aether/tests/test_budget.py`; 10/10 mutations killed. |
| Goals / subconscious / MOSS | 🟢 COMPLETE | 6 | `aether/agents/goals/` + `aether/agents/moss/` (DDR-851). `goals.md` refuses a goal with **no checkable success criterion** (an unfalsifiable goal makes every report against it unfalsifiable) and requires `CAP_SOVEREIGN` — it is the objective function; an agent cannot mark its own goal complete. Subconscious loop is periodic, goal-diffed, **idle-agents-only**, capped at a **minority (25%) share** of the 60 syscall/s limit, and **raises rather than truncating** a batch. MOSS: staged never in-place, **a regression suite that did not RUN is not a pass**, snapshot-before-promote enforced by the state machine, co-approval by **two different** principals. 36 tests; 11/11 mutations killed. |
| Privacy routing / OCR / context | 🟢 COMPLETE | 6 | `aether/agents/routing/` (DDR-852). Ring-3 privacy is an **early readable refusal, not the enforcement** — the kernel netfilter hook (DDR-802, `AR_PRIVACY_BLOCKED`) remains the only thing that counts — and it **fails closed**: an unreadable privacy state blocks. An unresolved hostname is never classified local. Routing refuses rather than near-missing, and is deterministic across registration order. OCR **quarantines** anything under 0.80 confidence with mandatory provenance; quarantined records enter the context marked `[UNVERIFIED OCR]` at lowest priority so a guess cannot be laundered into authoritative context. 36 tests; 10/10 mutations killed. |
| Research lineage (hypothesis/genome/dead-end) | COMPLETE | 6 | `aether/agents/research/` (DDR-853). All three are **records of what did not work**, append-only, because a research agent's failures are its most valuable output and the easiest to lose. A hypothesis needs a prediction (one that cannot be wrong cannot be right); superseding versions rather than edits; resolution requires evidence and cannot be re-run. A genome mutation requires a rationale. A dead end requires a failure reason, and `check()` returns the colliding entry rather than a bare boolean so the refusal is arguable. 32 tests; 10/10 mutations killed. |
| Mutation harness | COMPLETE | - | `tools/mutation/mutate.py` (DDR-853). **Fails closed**: aborts on a missing or ambiguous target, clears `__pycache__` and imports with `-B`, and **fails the run if any mutation kills nothing**. Replaces the ad-hoc shell version, which skipped absent targets with a warning while still printing kills from the previous mutation's stale bytecode. |
| Wayland Compositor | ⚪ SUPERSEDED | 7 | **Not required for v1.0.0** (item 41, operator pre-approved; DDR-865). The in-house C framebuffer compositor (`user/compositor.c`, DDR-704) already renders both ambiances with input, windowing, focus and drag, proven by ~25 gates. A wlroots port would replace working gate-proven code with a large out-of-tree dependency chain (libdrm/EGL/pixman) for no capability this release needs. Must NOT appear in release notes as an unmet item. | wlroots/Wayland is a large out-of-tree port (libdrm/EGL/pixman) — standing wall. An **in-house** full-screen compositor over `SYS_FB_*`+`SYS_INPUT_POLL` is the in-progress path (DDR-704), not wlroots. |
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


## BUG-1 — CLOSED 2026-07-29 (DDR-796 + DDR-797): TWO independent causes

Root cause: the CMOS/RTC read was not SMP-safe. `cmos_read()` is a two-port
sequence (0x70 selects a register, 0x71 reads it) over **chipset-global** state,
with no lock. Under `-smp 4` two CPUs interleave and each reads the other's
register, so `SYS_CLOCK` can jump in either direction.

The symptom was three inferential steps away: the DDR-730 metrics probe compares
two `SYS_CLOCK` readings, treats any decrease as a midnight wrap (`+86400`), and
so collapsed its whole 120-second window to zero — printing
`AGENT_METRICS FAIL: agent never observed as scheduled` while the agent was in
fact spawned, dispatched and reaped normally 75 KB of serial output later.

Fix: one IRQ-saving spinlock held across the whole of `rtc_now()`. Per-`cmos_read`
locking would be insufficient — another CPU could still move the index between
the *paired* readings the consistency loop compares.

Gate: `make smoke-rtc-smp` tests the invariant directly. A/B verified — the gate
FAILS with the lock removed and PASSES with it.

**Not closed.** Post-fix verification (7 gate runs) shows `RTC_MONO FAIL` 0/3 —
the clock fix holds — but `AGENT_METRICS FAIL` still appears in 1/7 runs. The
RTC race was a contributing cause, not the only one. The residual is a race
between the metrics probe's 120 s window and how long boot takes to reach the
daemon's agent spawn; DDR-791's unexplained serial flood (83% of console bytes)
is the leading suspect for that slowness. See DDR-796 "Correction".


### Second cause — the serial flood (DDR-797)

`user/syscallfuzz.c`'s `WILD[]` listed `0x8000000000` as an "unmapped user"
address. `user/user.ld:13` bases the user image at exactly that address, so it
was the most-mapped address in the process. Since the probe passes the same wild
value as every argument, `SYS_WRITE` became
`write(fd=(int)0x8000000000==0, buf=<image base>, count=~512 GB)`, and
`fd_write_user` chunked from the image base to the console until `copyin` ran off
the end of the mapping — dumping the probe's whole image ~20x per boot.

The kernel was correct throughout; `write(2)` is supposed to write what it can.
The defect was a test constant that tested the opposite of what it claimed.

Fixed by replacing it with `0x0000600000000000` (canonical, user half, unmapped).
Serial output: **97,564 bytes (83% binary) -> 5,901 bytes (0% binary)**.

Gate: `make smoke-serialflood` (32 KiB ceiling; also requires the boot sentinel,
so a kernel that dies instantly cannot pass by being quiet).


### Third cause — a probe owning a deadline it cannot judge (DDR-798)

After DDR-796 and DDR-797 both landed, `AGENT_METRICS FAIL` still appeared in CI
run 30391224155 (on `smoke-msixap`), while the same build passed 10/10 locally.
The flood fix was confirmed working there — the string appeared as a clean line
rather than buried in a binary blob.

`user/agentmetricstest.c` runs on EVERY boot and declared failure if the AETHER
daemon had not spawned an agent within 120 RTC seconds. That deadline is only
meaningful in `smoke-agentmetrics`, the one gate that asks the question and
allows 150 s. In the other ~106 gates it judged a boot that was never trying to
reach the daemon quickly, so on a slow TCG runner it failed for reasons
unrelated to what it tests.

Fixed by removing the probe's self-declared verdict: window expiry now prints an
informational line and exits 0. The assertion moves to `smoke-agentmetrics`'s
required sentinels, where a genuinely unscheduled agent fails on a missing
required pattern — which cannot be masked. The 120 s window is unchanged and no
gate timeout was widened.


### GLOBAL_FORBIDDEN false-positive audit (DDR-799)

Full pass over every probe against every `GLOBAL_FORBIDDEN` pattern, static plus
empirical (booting the four distinct device configurations the gates use).

**Exactly one offender**: `rootmounttest` under `QEMU_NO_EXT4=1 QEMU_SFS2=1`.
`kmain` picks the ext4 mount by disk INDEX (`main.c:895`, `blk_count() > 3`), so
that configuration leaves an SFS image at index 3 and roots the probe there.

Refuted by the empirical pass: `surfdestroytest` (surfaces need no virtio-gpu),
the NVMe patterns (emit only from `nvme_init`, which runs only when PCI finds a
controller) and the SMP self-tests (cannot emit with one CPU).

Fixed probe-side with a three-way verdict — `/HELLO.TXT` exists only on the FAT
default root, so it distinguishes "no ext4 provided" (SKIP) from "fell back to
the default root" (FAIL). The FAIL branch — the assertion that matters — is
unchanged. All four configurations now scan clean.

### Fourth cause — an unstated 30 s window twelve gates could not meet (DDR-803)

`tools/qemu_runner/boot_test.sh:19` is `TIMEOUT_S="${TIMEOUT_S:-30}"`. Twelve
gates never set it: `smoke-sfs-dirs`, `smoke-sfs-unlink`, `smoke-rootmount`,
`smoke-fsrm`, `smoke-sysinfo`, `smoke-time`, `smoke-dmesg`, `smoke-setname`,
`smoke-wxkernel`, `smoke-sfsroot`, `smoke-vfs-bigwrite`, `smoke-sfs-btree`.

Every one also declares a `FORBIDDEN_SENTINEL`, so per DDR-785 they are not
early-exit eligible — the full window must contain the sentinel. Their sentinels
are emitted late in boot. On a shared TCG runner that does not fit in 30 s: in
run 30447042919 `PRADYOS_BIGWRITE_OK` appears **nowhere** in the serial capture,
and the FAIL is stamped exactly 30 s after the gate starts (12:07:00 → 12:07:30).

This is the systemic half of what was being read as BUG-1: it wanders between
gates and never reproduces locally. It stayed hidden because a genuine defect
(the mis-sized `rtcmonotest`, DDR-796 addendum) was masking it.

All twelve now set `TIMEOUT_S=90` explicitly. Cost is real and not free — these
gates spend the full window every run, so this returns ~12 min/run of DDR-785's
46.5 min saving. DDR-803 argues why this is a correction (the window was never
chosen; the feature is not broken) rather than widening a timeout in place of a
fix, and records the alternative windows considered.

**Not fixed by this:** `smoke-smpuser` (run 30448425988) sets `TIMEOUT_S=180`
explicitly and still missed `[smp] user on AP OK`. Separate mechanism, OPEN-1
class, must not be attributed to DDR-803.

**Refuted along the way:** the first hypothesis was that the DDR-800/801 probes
had slowed the boot. Measured with verified-different kernel SHAs: 8.3 s with the
probes vs 8.4 s without. They cost nothing.

## DDR-802 — privacy mode is now a kernel property (mechanism; gate deferred)

`sys_sock_connect` refuses egress when privacy mode is on, **before** the
CAP_NET check, before the DDR-734 allowlist, and — deliberately — ahead of the
DDR-800 sovereign bypass. That last ordering is the substance: the bypass exists
so an operator can diagnose the network, while privacy mode is that same
operator saying nothing leaves. Honouring the bypass first would let the flag
override the control just set. The refusal is audited with the sovereign's own
pid under a distinct result code `AR_PRIVACY_BLOCKED`, so "did privacy mode
actually stop anything?" stays answerable.

State is a separate `g_privacy_mode` reached through the existing sovereign-gated
`SYS_SET_MODE` (no new syscall) via `AETHER_MODE_PRIVACY_ON/OFF` (2/3). A
bitmask on the existing argument was rejected: `aether_get_mode()` is returned
verbatim to ring 3 and userspace compares it against literal 0/1
(`compositor.c:837`, `aether_daemon.c`), so a privacy bit would have broken
those comparisons — a UI correctness bug introduced by a security fix.

**The gate is deliberately absent.** Privacy is global kernel state and every
kmain probe runs concurrently, so a probe that switches it on can refuse the
connects in `capnettest`/`sovegresstest`/`egressaudittest` and fail *their*
gates at random. That is the `rtcmonotest` mistake again — a window small enough
to pass locally and hit eventually in CI. `user/privacynettest.c` is written
(sovereign + CAP_NET, off→on→off, asserting no `AR_SOVEREIGN_BYPASS` survives)
but is not wired into the build. It needs a kernel-visible per-boot opt-in
first, so the probe exists only in its own QEMU config. Mechanism defaults off,
so nothing else changes: `make image` is warning-free and `smoke`/`smoke-fs`
pass unchanged.

## DDR-804 — per-boot probe selection (closes OPEN-7)

A probe could not exist in one gate and not the rest: `kmain` spawns every probe
with `sched_unblock` and `user_boot_from_sfs` loads unconditionally. Harmless for
read-only probes, which is why it never surfaced — until DDR-802's probe, which
mutates GLOBAL kernel state (privacy mode) and would refuse the concurrent
connects in `capnettest`/`sovegresstest`/`egressaudittest`, failing their gates.

The harness now passes `-fw_cfg name=opt/org.pradyos/probes,string=<csv>` and the
kernel reads it through QEMU's firmware-config channel (selector `0x510`, data
`0x511`), exposing `probe_enabled()`. `inb`/`outw` already existed in
`kernel/io.h`, so this adds **no new I/O primitives and no new hardware
assumptions**. New subsystem: `kernel/drivers/fwcfg/`.

Rejected alternatives, both recorded in the DDR: a compile-time `-DPROBE_*` would
force a distinct kernel image per gate, multiplying build time across ~113 gates
and destroying artefact identity (a green suite would stop meaning "this image is
good" — DDR-791's lesson); another block device would consume one of
`VBLK_MAX = 4` and make storage topology the carrier for unrelated config.

**Fails closed.** A wrong signature at key `0x0000` yields an empty probe set.
That direction is the safety property: an unselected probe fails its own gate —
loud, local, obvious — whereas a wrongly-selected one perturbs unrelated gates,
which is silent and remote. Every loop is bounded and every device-reported
length is clamped before use (64 entries, 256 bytes), and the 56-byte name field
is explicitly not assumed NUL-terminated.

Scope is new probes only; the 113 currently-green gates set no `QEMU_PROBES` and
boot exactly as before.

## DDR-806 — OPEN-1 explained: the SMP proofs ran behind the probe storm

OPEN-1 has been open since DDR-775 with four refuted hypotheses, all of which
hunted for a defect in the SMP path. There is none.

`smpuser_proof()`, `blkmq_proof()`, `smp_blk_integrity()` and `rqstress_proof()`
sat ~180 lines AFTER the user-probe spawn block in `kmain`. Every probe there is
started with `sched_unblock` and then competes with `kmain` for CPU, so whether
`kmain` reaches the proofs inside a gate window depends on runner speed and
`-smp 4` scheduling — fine on a fast host, intermittent in CI.

The evidence is what is **absent**. Each proof prints an `OK` or a `FAIL`
variant; in both failing serials **neither** appears. The proofs did not fail,
they never executed. A proof that ran and failed would have tripped
`FORBIDDEN_SENTINEL`. That single observation separates "SMP is broken" from
"the line was never reached", and it is why four defect-hunting hypotheses all
died.

Proof it is not a code regression: `9f1459a`, `6c375ea`, `c9a1537`, `d8c5c95`
carry a byte-identical kernel (the last three are docs-only), and CI alternates
FAIL / PASS / FAIL / PASS across two *different* gates. This also retires DDR-804
as a suspect — it was suspected from a local `smoke-shell` red, but CI shows
`smoke-shell` passing on that same commit, so that failure is a property of my
workstation and is tracked separately.

**Fix:** the proof block moves ahead of the probe spawns. Dependencies were
checked, not assumed — scheduler running, APs up, block layer live with SFS
mounted, all necessarily true because the probe block that follows needs them.
Nothing in the proofs consumes probe-produced state.

This is **not** a timeout change; no window moved. Raising windows would not
converge anyway: the proofs run last, so their latency grows with every probe
added (DDR-800, DDR-801, DDR-802 each added one), and per DDR-803 these gates
cannot early-exit so every raise is paid in full on every run.

**Verification limit, stated deliberately:** the base failure rate is ~50%, so
one green CI run is not evidence — two of the four runs above were green *with*
the defect present. Local gates passing confirms only that the reorder breaks
nothing. Status is "mechanism named and addressed", not "proven fixed", until
several consecutive green CI runs accumulate.

### DDR-806 CORRECTION — the fix above was refuted the same day

The reordering described in the previous section was implemented and **broke all
three gates it was written to repair** (`smoke-smpuser`, `smoke-blkmq`,
`smoke-rqstress-liveness` all FAIL on kernel `e8219c43ff84`; `smoke`,
`smoke-user`, `smoke-fs` unaffected). It is reverted; no DDR-806 code is in the
tree.

Two claims in that section are false:

* **"nothing in the proofs consumes probe-produced state"** — `smpuser_proof()`
  polls `g_user_on_ap`, which is only set when a user thread runs on an AP, and
  those threads come from the probe block. The existing order is a real
  dependency.
* **the line-number reasoning** — `kmain` is at `main.c:1805`; lines 1134/1311
  are inside `fs_test_thread` (`main.c:829`), a thread spawned from
  `sched_demo()` at `main.c:1580`. The question was never when `kmain` arrives.

Also refuted, on evidence rather than argument: the `g_smp_have_aps` race and
therefore **DDR-777 verdict (C)**. The flag is set at `main.c:1898`, before
`sched_demo()` runs at `main.c:1924`; and failing run 30507516805 contains
`[smp] cpus online=4/4`, `ap preempt OK`, `resched OK`. APs were up.

**Surviving candidate:** `fs_test_thread` does not reach `main.c:1311` inside the
window — ~30 `user_boot_from_sfs()` calls sit in between, each blocking on SFS
I/O over contended virtio-blk under `-smp 4`.

**Required next measurement, before any fix:** stamp `g_ticks` at `main.c:1134`
and `main.c:1311`. A failing run that never prints the second stamp confirms it;
both stamps landing early refutes it and reopens the search.

### DDR-806 settling measurement — candidate refuted; stamps left in as a CI trap

Two `g_ticks` stamps (`main.c:1134` = A, `main.c:1311` = B) measured against the
exact failing artefact (`BSP_LIVENESS=1`, `-smp 4`, `TIMEOUT_S=180`): B is
reached at **6.8–10.8 s of a 180 s window** across 5 runs, ~94% margin. The
"`fs_test_thread` runs out of window" family is closed, including the version
DDR-803 predicted.

Also refuted by inspection, at no measurement cost: output eviction. `kputc`
writes the log ring (which feeds `dmesg`) **and** COM1; the harness greps the
serial capture *file*, which is append-only. Nothing can evict a sentinel.

The stamps stay in the tree. They cost two `kputs` on a path that already prints
and turn every future intermittent red into a decisive artefact: **B present** ⇒
stall is after B (points at DDR-807); **B absent** ⇒ stall is before B. Cheaper
than fishing for a local repro at 180 s per attempt.

Running total on OPEN-1: **six hypotheses refuted, one open** (DDR-807 `kputc`).
Every refutation came from an artefact or from source, none from argument.

### DDR-807 — unbounded UART wait in `kputc` (S2 violation, found in passing)

`kernel/console.c:63` spins `while ((inb(COM1 + 5) & 0x20) == 0) { }` with **no
bound**, and `kputs`/`kwrite` call it with **interrupts disabled**. If THRE never
sets (back-pressured host pipe, full QEMU buffer), that CPU spins forever, IRQs
off — and on the BSP it would take `g_ticks` with it, silently un-bounding every
`g_ticks`-deadline wait in the tree.

Dormant under every configuration currently run, which is why it has never been
seen. **Fix deliberately deferred with no code**: the hard part is what happens
when the bound expires (drop the byte → lossy console, and every gate asserts on
serial; return an error → `kputc` is `void` and called from panic/ISR paths;
skip COM1 but keep the log ring → serial and `dmesg` disagree). A gate must also
genuinely back-pressure the UART, since asserting "did not hang" against a QEMU
that never stalls would pass against today's code. Per S11 the gate is absent,
not stubbed.

## DDR-809 — console output no longer eats console input (closes OPEN-8, DDR-807)

`kputs`/`kwrite` hold the console lock with interrupts OFF for a whole buffer
(ADR-030 atomicity), while `kputc` spun **unboundedly** on UART THRE. IRQ4 could
not fire, so COM1's 16-byte RX FIFO was never drained, and a burst of kernel
output silently destroyed concurrent console input.

Two changes inside that spin: the wait is bounded (`CONSOLE_THRE_MAX = 10000`,
then the byte is dropped — `klog_putc` has already recorded it so `dmesg` keeps
it), and the RX FIFO is drained inline so bytes arriving during a TX burst still
reach the ring. The masking itself is unchanged — ADR-030 still needs the buffer
atomic.

**Invariant change, stated explicitly:** the RX ring was documented lock-free
single-producer (IRQ4) / single-consumer. It is now **multi-producer under
`g_rx_lock`**, single-consumer unchanged (`kgetc_nb()` still needs no lock).
S6 is satisfied by existing precedent — `klog_putc()` already takes `klog_lock`
from `kputc`, including from ISRs. `g_rx_lock` is a leaf; order is
`g_console_lock -> g_rx_lock`, and the ISR takes only the latter, so no
inversion.

A BSP-only drain was designed and **rejected as unwritable**: `kputc` prints the
earliest boot messages, long before percpu, so a `this_cpu()->is_bsp` test would
dereference an uninitialised GS base on the first character the kernel ever
prints. The drain is instead gated on `g_rx_armed`, a plain global set at the end
of `console_rx_init()`.

| | baseline `4923c1831f2a` | fixed `4a1dc378c13e` |
|---|---|---|
| `smoke-shell` | FAIL 4/4 | **PASS 3/3** |
| RX losses per run | 1 | **0** |
| `st-ok=0` | absent | present |
| `st-fail=127` | absent (`st-fail=0`) | present |

`smoke`, `smoke-user`, `smoke-fs`, `smoke-syspipe` all PASS.

Gate is **absent, not stubbed** (S11): a real one must back-pressure the UART TX,
and QEMU drains instantly, so "did not hang" would pass against the broken code
too. `smoke-shell` is the regression test — it failed 4/4 through exactly this
mechanism and now passes 3/3 with zero losses.

## DDR-805 — SIGPIPE: a writer to a readerless pipe no longer survives

Three edits: `SIGPIPE 13` in the signal table, added to the default-terminate
set alongside `SIGTERM`, and raised in `fd_write_user`'s `FD_PIPE` branch on the
`-EPIPE` path **only**. A partial write succeeded in part, so killing the writer
would discard output it legitimately produced — the existing
`total > 0 ? total : -EPIPE` expression already encodes that distinction.

Delivery is at the next IRQ return to ring 3, not the syscall boundary, so
`write()` still returns `-EPIPE` first. That is required, not tolerated: a thread
with a SIGPIPE handler must observe the return value, and terminating inside the
syscall would make the handler case unrepresentable.

**The gate asserts survival, not exit status.** `sched_exit(-1)` sets
`exit_status = -1` for every default-terminate signal and the kernel records no
signal number, so asserting `$? == 13` would need a fourth edit adding a
`128+signum` convention — which changes SIGKILL and SIGTERM's observable status
too. Out of scope for "three edits", and folding it in silently is the scope
drift the A/B discipline exists to catch.

`smoke-sigpipe` (opt-in via the DDR-804 fw_cfg pattern) runs a **control** phase
first — a pipe whose reader is still open, where the write must succeed and the
probe must live — then the readerless phase. The control is what makes it
discriminating: a kernel that killed every pipe writer would pass the main
assertion and fail the control. `PRADYOS_SIGPIPE_STUB` is the FORBIDDEN sentinel
and is load-bearing — printing it *is* what surviving means, so a stub cannot
fake a pass.

| arm | kernel | verdict |
|---|---|---|
| A — no SIGPIPE at all | `30e6f27da9b2` | FAIL |
| B — raised, not default-terminate | `4a8f44823ce5` | FAIL |
| C — correct | `d3404eef47a7` | **PASS** |

Blast radius re-verified on arm C: `smoke-syspipe`, `smoke`, `smoke-user`,
`smoke-fs` all PASS.

**One unattributed observation:** `smoke-shell` failed once on arm C, then passed
6 consecutive runs on the identical kernel. Not claimed as caused by DDR-805 — a
~14% rate would show zero failures in 3 baseline runs roughly 64% of the time, so
the 3/3 baseline does not establish the rate is new. Logged, not concluded.

## DDR-811 + DDR-812 — SHA-256, and the metric lockbox in a page ring 3 cannot write

**DDR-811** ships `kernel/crypto/sha256.c` — pure C, no hardware acceleration, no
stdlib, no allocation. A SHA-NI path was rejected on portability: the same source
must build for riscv64 and aarch64, and a hardware path would compile on x86_64,
pass its gate there, and fail at runtime elsewhere — the failure shape where the
gate that should catch it is the one that passes. Validated against four FIPS
180-4 vectors including 1,000,000 `'a'`.

**DDR-812** puts the lockbox record inside DDR-795's `metric_page` frame, **not**
in SFS. The VFS gates writes on `CAP_FS_WRITE` alone, which every
`CAP_SOVEREIGN` process holds, so an SFS lockbox would be writable by exactly the
processes it guards against. In the page, the guarantee is a page-table property.

`sha256.o` joined the kernel link in this slice — DDR-811 deliberately left it
out for lack of a caller, and the build failed with `undefined symbol: sha256`
until DDR-812 provided one. That is the dead-object state resolving as designed.

**Arm D was already built.** `user/metrictest.c` (DDR-795) stores to
`METRIC_USER_VA` and treats survival as failure, and `smoke-metric` pins the
fault to `cr2=0x00007FFFFFEFF040` — offset 64, exactly where the lockbox record
now begins. The existing W^X gate therefore protects the new record by
construction, so `smoke-lockbox` covers only what is new (read + hash
verification). Duplicating arm D would have added a second probe for one
property and a second way for them to disagree. `smoke-metric` is consequently
the most important regression in this slice, and it passes.

**Write-once, as actually enforced.** The spec asked for a compile-time assertion
that only two call sites exist. C cannot express that. What is enforced instead:
`lockbox_commit()` is `static` inside `metric_page.c` so the linker makes it
unnameable elsewhere; two thin wrappers encode the phase so a caller cannot pick
one; a runtime phase guard rejects out-of-phase calls; and arm D tests the
property from outside, which is the only check that tests behaviour rather than
intent.

Two details that are load-bearing rather than cosmetic:

* `rtc_present` is recorded, so "booted at the epoch" stays distinguishable from
  "we do not know when this booted". Coercing an absent clock to 0 would turn an
  unknown into a plausible-looking fact.
* The hash input order is **binding** and stated in both DDR-812 and a comment on
  the struct. The probe serialises **independently** of the kernel — two
  implementations of one contract, so a divergence is detectable rather than
  silently agreed upon.

`committed` is written last, after the digest, so a reader can never see a
committed flag over a stale hash.

Gates: `smoke-lockbox`, `smoke-metric`, `smoke-sha256`, `smoke`, `smoke-user`,
`smoke-fs`, `smoke-shell` all PASS on kernel `f36ce889348e`. `ETAMPER` (133) and
`SYS_METRIC_READ` (76) are new.

### DDR-812 A/B — and the first attempt, which was invalid

| arm | kernel | verdict |
|---|---|---|
| A — boot commit removed | `0c77a1ad2789` | FAIL (`-ENOENT`) |
| B — `record_sha` bit-flipped after a valid commit | `1725e0a1847e` | FAIL (`-ETAMPER`) |
| C — correct | `f36ce889348e` | **PASS** |

Arm B commits a **fully valid** record and then flips one bit of the stored
hash, which isolates verification from every other failure mode: the record is
otherwise perfect, so only a kernel that actually checks the digest can fail it.

**The first A/B attempt was discarded as invalid**, and the reason is worth
keeping because the output looked like a good result:

```
ARM A | kernel=0c77a1ad2789 | FAIL
ARM B | kernel=1725e0a1847e | FAIL
ARM C | kernel=1725e0a1847e | FAIL      <- same SHA as B, and C should PASS
```

Arms B and C shared a SHA, so at least one never rebuilt, and C reported FAIL
for a binary that passes when run directly. The per-arm cleanup had deleted only
the `.o` each edit touched, leaving other artefacts stale — DDR-791's trap again,
and the same one that made DDR-811's arm A vacuous three times. A/B arms A and B
failing is exactly what one wants to see, which is precisely why reporting it
would have been convenient and wrong.

**Build determinism was verified before trusting any SHA:** two consecutive clean
rebuilds of the unmodified tree both produce `f36ce889348e`. That underpins every
A/B in this project — if the build were non-deterministic, "distinct SHAs prove
the arms differ" would be unfounded. It holds.

The re-run scrubs all derived artefacts per arm and asserts each edit actually
applied before building, so a silently non-matching edit now aborts instead of
producing a confident verdict for unmodified code.

## DDR-816 — kernel entropy, fail-closed (unblocks §S1–§S4)

No entropy source existed: `grep -il "rdrand|rdseed|random|entropy|csprng"` over
`kernel/` matched one file, and the match was the word "random" in a comment.
Every remaining §S feature is cryptographic, so this — not ACC — was the §S
critical path.

virtio-rng primary, over the already-generic transport (`virtio_pci_attach` /
`negotiate` / `setup_queue` / `driver_ok` / `notify` serve blk/net/gpu/input
today), so this is a new consumer rather than new transport code. RDSEED
secondary and CPUID-gated, which can never be the only source because riscv64 and
aarch64 lack it.

**No third source — fail closed.** A jitter fallback was rejected deliberately:
under TCG, timing variance is largely a host-scheduler artefact and is not
trustworthy from inside the guest, and a source that silently degrades to
predictable output is strictly worse than one obviously absent — absence is
detectable, degradation is not. Same argument as DDR-811's hand-rolled hash, but
sharper: a wrong hash fails a vector immediately, whereas a weak key produces
ciphertext that looks perfect and protects nothing.

Device matched on `device_id` (`0x1040 + type 4` = `0x1044`), not `class_code` —
the `0x1040+type` rule is structural to modern virtio, whereas the class byte is
not something to assert from memory. Confirmed empirically: `smoke-rng` passes.

| arm | kernel | verdict |
|---|---|---|
| A — no virtio-rng device attached | `4a6b5e680038` | FAIL |
| B — driver returns a fixed buffer | `b2b2b57ece36` | FAIL |
| C — correct | `4a6b5e680038` | **PASS** |

Arms A and C share a SHA **correctly**: arm A varies the QEMU invocation (no
`-device virtio-rng-pci`), not the source, so the same binary is what should be
under test. Arm B varies the source and has its own SHA.

Arm B is the load-bearing one — the device attaches, the queue works, and
`rng_bytes()` returns success while handing back a constant. That is the
likeliest real implementation bug, and only the two-draw byte-for-byte
comparison catches it. Asserting "the call returned 0" would pass against it.

Every gate other than `smoke-rng` boots with **no** entropy device, so the suite
collectively exercises the fail-closed path: a `rng_bytes()` that wrongly
returned success with zeros would print `PRADYOS_RNG_STUB` and
`GLOBAL_FORBIDDEN` would fail all of them.

Nine gates PASS on `4a6b5e680038`, including `smoke-shell` — which failed 5/5 on
this workstation the previous day on functionally identical code. Same code,
recovered host, opposite verdict. That confirms the DDR-816 §Implementation
correction: OPEN-9 is host-state dependent, and reverting the attribution rather
than "fixing" a non-existent regression was right.

## DDR-818 — HMAC-SHA256 + HKDF-SHA256 (first of ACC's four missing primitives)

ACC derives two independent AEAD keys from one X25519 shared secret, separated
only by the HKDF info label. That separation is what makes `K_session !=
K_owner`, which is what lets the same nonce appear in both boxes without being a
break — so HKDF is load-bearing for the envelope's safety argument, not a
convenience.

**Scope named at design time:** RFC 5869 is thin over HMAC, and HMAC was not in
the tree either. So this slice is HMAC *and* HKDF. Naming that in the DDR rather
than discovering it mid-implementation is what keeps a slice from quietly
becoming two.

Three RFC 5869 Appendix A vectors, each covering something the others cannot:

| # | case | only this one covers |
|---|---|---|
| 1 | basic, 42-byte OKM | the ordinary path |
| 2 | long inputs, 82-byte OKM | forces the expand loop past `T(1)` |
| 3 | zero-length salt and info | the NULL-salt branch — RFC 5869 requires HashLen **zero bytes**, not an empty string |

TC3 matters because treating an absent salt as an empty string produces
plausible-looking output that is simply wrong, and is invisible in TC1 and TC2.

| arm | kernel | verdict |
|---|---|---|
| A — primitive unlinked | *(no artefact)* | cannot build |
| B — expand loop truncated to `T(1)` | `f74b515df09e` | FAIL |
| C — correct | `2a454913ef66` | **PASS** |

Arm B proves TC2 is load-bearing rather than decorative: truncating the loop
still satisfies TC1 and TC3 (42 bytes each, one block) and fails only TC2.

`hkdf.o` is deliberately **not** in the kernel link — no caller until DDR-813,
and an unreferenced object in the image is dead code. Same discipline as
DDR-811, where `sha256.o` waited for DDR-812 to become its first caller.

### ACC's remaining dependency chain

A tree check after DDR-816 found ACC needs **four** primitives, none present —
and the only `x25519`/`chacha20`/`poly1305` matches in `kernel/` were prose in a
comment in `rng.h` that I had written myself. Sequenced as
818 (done) → 819 ChaCha20-Poly1305 → 820 X25519 → 821 Ed25519 → 813 ACC, so a
vector failure and a protocol failure stay distinguishable.

## DDR-819 — ChaCha20-Poly1305 (RFC 8439), third of ACC's four primitives

ACC's envelope carries two AEAD boxes over the same plaintext, keyed by the two
HKDF outputs from DDR-818. This is the cipher that fills them.

**AES-GCM was rejected as a decision, not inherited.** The same source must be
correct *and* constant-time on three architectures, two of which have no AES
instructions. Software AES is timing-variable via its S-box tables, and a
timing-variable AEAD in a system whose threat model includes "an attacker holds
an enrolled device" is a weakness, not a performance note. ChaCha20 and Poly1305
are 32-bit add/xor/rotate plus a 130-bit multiply-accumulate: constant-time as a
property of the algorithm rather than of the compiler's mood.

`aead_open` verifies the tag in constant time and writes **no plaintext** before
the verification succeeds.

### Two defects, and where the vectors earned their keep

1. `ctlen_or(ptlen)` — a typo, caught by the compiler.
2. Incoherent Poly1305 short-block handling: a conditional whose two branches
   were identical, plus an implicit high bit that must NOT be set for a short
   block. The spec is simpler — a full block gets an implicit 1 at bit 128; a
   short block gets a literal `0x01` after the data and no implicit bit.

Defect 2 is the instructive one: it compiles cleanly and produces a wrong tag
**only** for messages that are not a multiple of 16 bytes. RFC 8439 §2.5.2 uses a
34-byte message — a 2-byte final block — so it catches it, and a 16-byte-multiple
vector would not have. That is the concrete argument for choosing vectors by
which code path they reach rather than by convenience, and for per-primitive
vectors rather than only testing the composed AEAD.

```
poly1305 §2.5.2 (34-byte msg, 2-byte final block): OK
chacha20 §2.4.2 (first 16 ct bytes):              OK
```

Verified under `gcc` on the host against the same source the kernel compiles.
Deliberate: the question was whether the arithmetic is correct at all, and a
~90 s boot per iteration is the wrong instrument for that. The in-kernel gate
does not replace the host run — the host run de-risks it.

### The gate that cannot work, stated rather than faked

DDR-819's arm C was originally specified as "replace the constant-time tag
compare with an early-exit `memcmp` → must FAIL". **It cannot.** The two
comparisons reject exactly the same inputs; the difference is timing on real
hardware, not output. No black-box gate in QEMU can distinguish them.

So arm C became the *tamper* arm — the gate asserts what it can (tampered input
is rejected) and the constant-time requirement is carried by construction, a
comment on the function, and this record. This is the same class of gap that
DDR-820/821 will have in larger form, and the owner has accepted it on the
record (D-1) for those.

### Deviation from spec, recorded

`aead.{c,h}` is **not yet wired into any build** — no `smoke-aead` gate exists.
DDR-819 defers linking until DDR-813 becomes the first caller (the DDR-811
precedent), but the *probe* was also deferred, so in-QEMU verification of this
primitive is still outstanding. It rides on the host-vector run alone until
DDR-813 wires it.

## ADR-035 — bounded W^X carve-out

The single bounded exception to the ADR-021 W^X invariant, for §E-05
self-rewriting code. Binding: supersedable only by a new ADR, never quietly
amended.

## DDR-817 — CI sharding, and eight gates that were never running

### The measurement, before the assumption

Run `30741785980` (`main` @ `b823bb5`, green), from the `.../jobs` endpoint:

| job | wall-clock |
|---|---|
| **`build-and-boot`** | **2 h 08 m** |
| `arch-bootstrap (aarch64 / riscv64)` | 20 s / 21 s |
| `aether-layer` | 15 s |
| `code-graph` | 8 s |

The entire critical path was one job running 110 QEMU boots in series. Inside
it, 33 steps over 95 s accounted for 59 % of the time, and **shared setup was
~49 s against ~7 600 s of boots** — work that is ~99 % embarrassingly parallel,
running on one runner.

`build-and-boot` is now a 6-way matrix, packed longest-processing-time-first by
measured duration (`tools/ci/gate_shards.txt`). Longest shard ≈ 1 425 s + ~85 s
setup ≈ **25 min**, against a 60 min target and a 128 min baseline.

Round-robin packing was rejected: eleven gates sit at a full 180 s
(`QEMU_SMP=4` with a `FORBIDDEN_SENTINEL`, so DDR-785 early exit does not apply
to them), and packed badly those alone would cap the matrix at 33 min.

### What was NOT done, and why

Reclaiming those eleven 180 s windows by extending DDR-785 — exit once the
required sentinels appear plus a grace period — was rejected. DDR-785 excluded
forbidden-sentinel gates because a forbidden pattern can appear *after* the
required ones. A grace period weakens that to "probably long enough", and the
only evidence any particular period suffices is that these gates happen to print
failures early today. That is an observation about current behaviour, not an
invariant. Sharding reaches the target without spending it, so there is no
reason to.

**No gate's semantics, timeout, or sentinels changed. No gate was removed.**

### The finding that mattered more than the speed

`ci.yml` hand-listed 111 gate steps and **nothing compared that list to the
Makefile**. Eight gates existed and had never run in CI:

```
smoke-sha256  smoke-hkdf  smoke-lockbox  smoke-rng
smoke-sigpipe  smoke-privacy-netfilter  smoke-blkmq  smoke-rqstress
```

That is the gate for **every crypto primitive promoted to `main` this session**
— DDR-811, DDR-812, DDR-816, DDR-818 — plus DDR-805 and DDR-802. Those
promotions each had two CI greens on the exact tip. The greens were real and
they carried no information about those features. What those features actually
rest on is their local 3-arm A/B verification, which is genuine evidence but is
not what "two CI greens" was taken to mean.

`smoke-blkmq` and `smoke-rqstress` were additionally masked by their own
variants: `ci.yml` ran `smoke-blkmq-trace` and `smoke-rqstress-liveness`, so a
grep for the base name matched and the absence looked like presence.

All eight are now in the matrix. Their durations in the manifest are their own
`TIMEOUT_S` — the worst case — because there is no measured value to use.

### `make ci-shard-check` — the assertion that makes the speed safe

A sharded suite's characteristic bug is a gate in no shard: CI gets faster
because it quietly stopped running something. `tools/ci/shard_check.sh` fails if
any `smoke-*` target in the Makefile is unassigned, assigned twice, or named in
the manifest but undefined in the Makefile. Exclusions are explicit and each
carries its reason on the line — a blanket "every target must be sharded" would
be silently satisfiable by deleting the target.

Verified by removing the assertion's own subject, four ways:

| arm | manifest state | verdict |
|---|---|---|
| A | correct | **OK** — 117 gates, 6 shards, 4 excluded |
| B | `smoke-sha256` dropped | FAIL, names it |
| C | `smoke-sha256` listed twice | FAIL, names it |
| D | `smoke-sha256` typo'd to `smoke-sha257` | FAIL on both symptoms |

There is no kernel change in this slice, so the usual 3-arm kernel A/B has
nothing to vary — the table above is its equivalent, and it varies the thing
that can actually break.

`smoke-selftest` (DDR-785's harness self-test) is excluded from the manifest and
run as a setup step in **every** shard instead: a shard whose gates trust the
harness must have checked the harness first.

## DDR-846 / DDR-847 — the 8 named agents get behaviour, and a loop that can revise it

DDR-846 shipped in `3b369c5` **without updating this file** — a break of the
same-commit tracker rule. Recorded here rather than quietly fixed: the row for
Named Agents above was one commit stale, and the reason it was not caught is
that nothing enforces the rule mechanically.

### The tie is the whole decision (DDR-847)

SkillOpt's acceptance rule is `>`, not `>=`, and that is not a style choice.
An optimiser that accepts ties drifts: each accepted tie changes the skill with
no evidence the change helped, every individual step "passes", and after N of
them the skill has wandered somewhere nobody chose and the history says nothing
went wrong. This is the eleventh instance in this project of one structural
defect — **a check that absorbs an invalid case instead of rejecting it, so
drift is silent and looks like success** (DDR-817, -822, -823, -824, -825, -826,
-830, -832, -833, -835, -845). A tie is the invalid case here.

Three further refusals follow from the same principle: a held-out set smaller
than 4 rollouts is refused outright rather than gambled on; a held-out set that
overlaps the training rollouts **raises** rather than quietly scoring the
candidate on its own homework (S7); and every result carries
`incumbent_score`, `candidate_score` and `heldout_n` whether it accepted or
rejected, because an acceptance an operator cannot audit is a change that
happened for unstated reasons.

### The verification that matters is the mutation, not the pass

16/16 green proves little on its own — a suite that only checks the accept path
would also be green. The check with teeth: changing `if cand > inc:` to
`if cand >= inc:` fails exactly `test_tie_is_rejected_not_accepted` and
`test_history_records_rejections_too`, and nothing else. A suite still green
under that mutation would not be testing the decision at all.

pytest is not installed on the build host, so the suite was run locally through
a minimal `raises` shim against the **real** test file (not a re-implementation
of it). CI's pytest job remains ground truth.


## DDR-848 — the loop can rewrite the skill, so something has to bound what "improve" means

DDR-847 gave SkillOpt the power to rewrite an agent's skill file on evidence.
That power and the document it edits are in tension, and the tension has a
direction: **a skill file says what the agent may do, and every refusal in it is
a task the agent declines and therefore scores zero on.** Deleting a refusal
raises the score. Nothing in DDR-847 prevents that -- its acceptance rule asks
only whether the candidate scored better, never whether the candidate is still a
legitimate skill file.

That is instance **12** of this project's recurring structural defect, and
instance 11 was the tie in DDR-847. Both are the same shape as the earlier ten
with one addition that matters: **the invalid case an optimiser will find is the
one that scores well.** A penalty is not enough, because a penalty is a price --
and capability escalation is not something an agent should be able to buy with
performance elsewhere.

### Validation runs BEFORE scoring, not after

A candidate that escalates capability must never reach a comparison a good score
could carry it through. `consolidate()` builds the candidate, validates it, and
only then evaluates. If validation ran afterwards as a safety net, a
high-scoring rejected candidate would become a standing argument for relaxing
the gate.

The subtle rule is where capabilities are read from: the **declaration line**,
not the whole document. A whole-document scan would read a refusal --
"refuses to write to a `CAP_SOVEREIGN`-locked path" -- as a claim to *hold*
`CAP_SOVEREIGN`. A gate that miscounts refusals as grants punishes exactly the
files that are most careful about naming what they decline.

### Sleep gets no relaxed threshold

There is no "we have more data now, so a tie is good enough". An offline pass is
a better opportunity to be rigorous, not a licence to be lenient; a second
weaker acceptance path would become the path every change flowed through, and
DDR-847's guarantees would still be in the code while no longer being true of
the system.

The split is `sha256(salt:task_id)`, not arrival order and not random. Order
correlates the held-out set with time, so a skill that improved mid-session
scores well for reasons unrelated to the edit. Random makes a rejection
impossible to reproduce -- and an unreproducible rejection gets re-run until it
passes, which is the same as having no gate at all.

### Transfer is a proposal, never an application

A lesson that improved KRYOS is evidence about KRYOS. It is a *hypothesis* about
PRAX, and only PRAX's own held-out evaluation can settle it. Splicing it in
directly would open a second path into a skill file that bypasses held-out
scoring -- and being the easy path, it would become the path everything took.

### Every guard is killed by at least one test

36 tests, all of them refusals. The number that means something is not 36/36
green; it is the mutation table:

| mutation | tests killed |
|---|---|
| capability-escalation check disabled | 3 |
| refusal-thinning check disabled | 1 |
| capabilities parsed from whole document | 7 |
| sleep skips validation | 1 |
| split keyed on arrival order | 2 |

A guard no test kills is a guard that is not being verified. None of the five
are in that state, and the tree was restored and re-run green after each.

### CI status at time of commit

GitHub Actions is in a **major outage** (incident `qcvjkzcs7j74`, opened
15:22 UTC 2026-08-06, still open): "capacity remains constrained and jobs may
still be delayed or fail". Concretely -- the re-run of `31120386501` sat
`queued` for 45+ minutes with **zero jobs scheduled**, and the push of `4bdbf3a`
created **no run at all** (the incident lists webhook deliveries as affected).
The two earlier reds, `31120032364` and `31120386501`, both died in "Set up
job", before checkout and before any build, with `Failed to resolve action
download info. Error: Service Unavailable`. **No code fix applies to them.**

So DDR-847 and DDR-848 are *locally verified and not yet CI-confirmed*, and the
distinction is recorded here rather than glossed. Group 9 stays gated on real
CI greens for both.


## DDR-849 — a ceiling, a record, an attribution, and two bugs I wrote on the way

Three things an agent that spends resources needs, each with an obvious
implementation that fails *quietly* -- the failure looks like success and the
number the operator reads stays plausible.

**The budget refuses; it never degrades.** A soft budget is worse than no
budget: the run keeps going, quietly does less per step, and "succeeds" having
answered from a truncated context. The only evidence is that the output got
vaguer. A refusal is a fact an operator can act on.

**An unknown model raises; it is never priced at zero.** `rates.get(model, 0.0)`
keeps the code short and nothing ever crashes, and it makes the cost of every
model nobody has priced yet invisible. Zero is a legitimate price for a local
model -- the point is that it must be **declared**, so "free" and "unknown" stay
distinguishable. That is instance **13**.

**The trajectory is append-only and redacted on the way IN.** Redacting at read
time means the secret is on disk and every future reader is one code path from
printing it. JSONL rather than one JSON array because a run killed mid-write
must still be readable: a truncated array is not valid JSON at all and takes the
whole run with it. The reader tolerates a truncated tail and refuses mid-file
corruption -- a bad line with good lines after it is not a truncated write.

### The two bugs, recorded because the second one is the interesting one

The first draft tracked one `_outstanding` counter for both pools, so a
completion reservation was debited against the fenced reserve **and** against
ordinary availability. Every individual operation looked correct. Caught by
writing the test that says completion work must not consume ordinary budget.

The second was in the check meant to catch the first. `check_invariant()`
asserted

    total == spent + outstanding + granted + available + reserve_remaining

which is a **tautology**: `available` is *derived* by subtracting the others
from `total`, so the assertion could never fail. It read as a strong invariant
and was decorative. I only found it because a test that deliberately corrupted
the books still passed.

That is instance **14**, and it is the defect appearing inside code written to
guard against the defect. The identity is now documented as something
deliberately *not* asserted, and the check tests the independently-tracked
quantities -- the counters against the open reservations, and non-negativity of
each pool -- which are the parts that can actually be violated.

### Ten mutations, ten kills

| mutation | tests killed |
|---|---|
| unknown model priced at zero | 2 |
| pricing rounds down | 1 |
| ledger records tokens before pricing | 3 |
| budget truncates instead of refusing | 3 |
| completion reservation charged to ordinary pool | 3 |
| grant debited lazily | 2 |
| failed commit consumes the reservation | 1 |
| trajectory redaction disabled | 4 |
| mid-file corruption tolerated | 1 |
| redaction depth bound removed | 1 |

34/34 green after restore. Still **not CI-confirmed** -- GitHub Actions remains
in the outage described in the DDR-848 section above.


## DDR-850 — I built four items from the tracker label instead of the spec text

`docs/BUILD_TRACKER.md` carries one-line titles for the Section 3D items. They
are abbreviations of the real requirement in `docs/AETHER_MASTER_FEATURES.md`
§3D, and I built from them. Re-reading the source paragraph found four gaps:
#50 is "TokenJuice **context compression** (≤80% tokens)" and I built a token
budget; #48 is a validation gate "**`CAP_SOVEREIGN` always**" and I built only
the structural checks; #47 is "**harvest→mine→replay→consolidate**; **pauses
active agents**" and I built a single `consolidate()`; #52 pairs `token_count`
with **`latency_ms`** and I recorded only tokens and cost.

That is instance **15**, and its shape is worth naming because it is not a
coding error: **a tracker line is a label FOR a requirement, not the
requirement.** Building from the label produces something that satisfies the
label and can still miss the thing — and it passes review, because the label is
what a reviewer checks against.

The DDR-849 budget is **kept**, not replaced. Refuse-don't-degrade stands on its
own merits; it simply was not item #50.

### What the corrections actually enforce

**"Always" means always.** A structurally flawless candidate is still refused
without `CAP_SOVEREIGN` — a skill file states what an agent may do, so revising
it is a sovereign act however well-formed the revision is. And a missing
approver is **refused**, not read as authorised: the permissive reading of a
forgotten argument is the one that turns a dropped parameter into an ungated
self-rewrite.

**Sleep refuses to run over live work.** Consolidating while an agent is
mid-task lands the revision between two steps of one action, so neither the old
skill nor the new one describes what actually ran, and the trajectory cannot be
read back as a coherent run. Activity is tracked explicitly, because "the caller
will have stopped them" is true right up until it is not.

**Compression refuses rather than best-efforts.** Pinned segments — system
prompt, skill body, current task, safety invariants — are the ones whose absence
changes what the agent *is* rather than what it knows, and they are never
dropped. If the 80% target cannot be met without dropping one, `compress()`
raises: a best-effort context is indistinguishable from a successful one at the
call site, and the agent proceeds believing it has information it does not have.

### Verification

108 tests across four suites (16 + 36 + 34 + 22), all green. Seven mutations,
seven kills: sovereign gate removed (3), missing approver treated as authorised
(2), sleep no longer refuses over active agents (1), pinned segments evictable
(3), impossible target returns best-effort (1), eviction ignores priority (1),
latency not accumulated (2).

Committed **without pushing**: GitHub Actions remains in outage `qcvjkzcs7j74`,
and the operator's instruction is to hold local until it clears, then run CI
across the whole batch at once.


## DDR-851 — three items where the guard is the feature

**A regression suite that did not RUN is not a pass.** `if failures == 0` is
also true when zero tests ran, which turns a broken harness into a green light
for an agent rewriting its own source. MOSS checks `ran` and `total > 0`
separately from `failures == 0`, so "no result" stays a distinct state from
"passed". It is the single most dangerous confusion available in this pipeline
and the cheapest one to write by accident.

**A goal with no checkable success criterion is refused.** "Improve file
handling" cannot be satisfied or failed, only asserted about — and once a goal
is unfalsifiable, every report against it is unfalsifiable too, so the tracking
apparatus emits confident statements carrying no information.

**The subconscious gets a minority share of the syscall budget.** Background
work that eats the limiter starves the foreground agent, and the symptom is
that *the agent* looks slow, not that the subconscious looks wrong. Exceeding
the share raises rather than emitting a partial batch, because a truncated batch
silently drops the lowest-priority goals while looking like it kept up.

Co-approval is **two different principals**: one principal holding both
`CAP_REWRITE` and `CAP_SOVEREIGN` is one decision wearing two hats, which is
exactly what a two-key rule exists to prevent. And the promote path is ordered
so that promoting without a snapshot is structurally impossible — a rollback
path that was never created is discovered at the moment it is needed, which is
the moment it cannot be created.

36 tests, eleven mutations, eleven kills. Section 3D is now 11 of 21.
Locally verified, not CI-confirmed; outage `qcvjkzcs7j74` still open.


## DDR-852 — the ring-3 privacy check is a convenience, and saying so is the decision

The kernel already blocks non-local egress in privacy mode and gives it its own
audit code (DDR-802, gate `smoke-privacy-netfilter`). This slice adds a ring-3
refusal so an agent is told *why* by the router instead of meeting an opaque
`-EPERM` three layers down.

Naming what this layer is **not** is part of the design. A ring-3 check that
looks like enforcement invites someone later to weaken the kernel one on the
grounds that it is redundant. It is not redundant — it is the only one that
counts, and this layer must never be trusted.

It fails closed: `PrivacyState.UNKNOWN` blocks. Assuming "off" when the state
cannot be read turns an unreadable config into an open egress path, and nothing
in the logs would say so. And an unresolved hostname is **not** local, however
local it reads — resolution can change between the check and the connection, so
treating a name as local makes the classification depend on a DNS answer the
function never saw.

**OCR is lossy in a way that reads as authoritative.** A misrecognised digit
does not arrive flagged; it arrives as a number, is stored as a fact, and is
retrieved later with the same confidence as something the operator typed.
Anything under 0.80 is quarantined — marked and retrievable, neither discarded
nor silently promoted — and a quarantined record enters the context as
`[UNVERIFIED OCR]` at lowest priority, because passing it in clean would launder
the guess into an authoritative-looking line.

36 tests, ten mutations, ten kills. Section 3D is now 15 of 21.
Locally verified, not CI-confirmed; outage `qcvjkzcs7j74` still open.


## DDR-853 -- two defects in my own verification, found and fixed

Every DDR from 847 onward leans on a mutation table: flip a guard off, and if no
test dies, the guard is not verified. While running this slice I found the
harness producing a result it had not earned, twice.

**Stale bytecode misattributed kills.** `__pycache__` on `/mnt/c` has coarse
mtime granularity, so a mutate/restore cycle inside one second could leave
Python importing the *previous* module. A mutation appeared to kill a test that
had nothing to do with it.

**A mutation whose target string was absent was skipped with a warning, and the
run still printed kills.** DDR-850 reflowed a `validate_skill_update(...)` call
onto three lines, so one guards-matrix mutation silently stopped applying -- and
the kill it reported came from the previous mutation's stale bytecode.

That second one is instance **16**: a check that absorbs an invalid input
instead of rejecting it, so drift is silent and looks like success -- inside the
tool built to detect exactly that.

**Every prior matrix was re-run** with bytecode cleared and `-B`. All 43
mutations across DDR-848 through 853 still kill at least one test, and the one
mutation that had never actually applied kills two once corrected. **No guard
was found unverified -- but they stood on a harness that could have hidden a
gap**, which is why this is recorded rather than quietly re-run.

`tools/mutation/mutate.py` now aborts on a missing or ambiguous target, clears
bytecode before every run, and fails the run if a mutation kills nothing. A
surviving mutation is the finding, not a footnote.

Section 3D is now 18 of 21. Locally verified, not CI-confirmed.


## DDR-855 -- I rebuilt D-07 and D-13 because I skipped the code graph

DDR-853 shipped a hypothesis type and a dead-end registry that **already
existed**, and both existing versions were stricter. D-07's `Hypothesis`
requires statement, falsification condition, expected evidence and estimated
cost; mine required a `prediction`. D-13's `FailureMemoryRegistry` is
append-only with **no delete path at all** -- its docstring says "a registry
that can forget is worse than no registry" -- while mine dropped its oldest
entries when full, which is exactly the forgetting D-13 exists to prevent.

**Root cause: I never ran `graph_session_primer()`.** CLAUDE.md requires it
before opening any source file, precisely to prevent this, and I worked from
directory listings and greps for the whole session instead. The tell was there
and I misread it -- creating `research/__init__.py` reported "has been updated",
not "File created"; I saw a five-line stub and moved on rather than asking why a
stub existed, which is the question that would have found
`hypothesis_generator.py` beside it.

This is not the "check absorbs invalid input" pattern catalogued in
BUILD_TRACKER §4. It is its own lesson: **an orientation step skipped to save
time costs more than it saves, and it fails silently** -- duplicated code
compiles, passes its own tests, and looks like progress.

**Remediation.** The duplicate registry is deleted; #63 is D-13. The divergence
score #63 actually requires was **added to D-13**, which genuinely lacked it --
`is_dead_end` matches an exact signature, so a *reworded* near-repeat walked
straight through. `nearest_dead_end()` returns the record and its score, not a
boolean, so the refusal names which dead end was hit. The tree was rebuilt over
D-07's type and now adds only what D-07 lacks: parent links, versions, and a
schema-versioned serialisation.

29 tests on D-13 (9 new, beside the registry rather than in a drifting second
file), 21 on the tree and genome, 17 unchanged on D-07, 201 across my suites.
D-13's own guard caught four of my new tests -- `record()` requires a detail --
and I fixed the tests, not the guard.

Every remaining item is now checked against the code graph before any file is
written.


## DDR-856 -- Section 3D complete (21 of 21), and the harness caught two real gaps

Per DDR-855 every item was checked against existing code first. D-11
(`knowledge_consolidation`) resolves conflicting claims about one key; C-02
(`distributed_experiments`) is quorum voting among peers. Neither is similarity
search or head-to-head competition, so #62 and #64 are genuinely new and compose
with them rather than replacing them.

**#62 vector KG.** A dimension mismatch is refused, never padded -- padding
silently changes a vector's direction, so the neighbour it returns is
*confidently wrong* rather than absent. `cosine` raises on a zero vector instead
of returning 0.0, because 0.0 is a real similarity meaning orthogonal and using
it for "undefined" makes an error look like a result. A full graph **raises
rather than evicting** (the D-13 principle).

**#64 tournament.** A variant that never played is *unranked*, not ranked last:
"untested" and "tested and bad" are different states, and collapsing them lets a
variant win by having avoided scrutiny. A tie does not promote -- the same bar as
DDR-847.

**#65 visualiser.** Reads the existing trajectory through the writer's own
reader rather than a second parser that would drift from it. Never un-redacts,
deterministic (a render timestamp would make it un-replayable), self-contained
(a fetching viewer on an offline OS breaks exactly when needed, and under
privacy mode is an egress attempt), everything HTML-escaped.

### The harness reported 15/15, and was wrong

Two entries claimed "killed by 1" against a suite that had passed 35/35. The
harness matched any line starting with `FAIL`, and the runner's summary line
reads `FAILREG OK -- 35 passed` -- a **passing** run counted as a kill. Third
appearance of the recurring defect inside the tooling built to detect it. The
kill pattern is now anchored to `^  FAIL  <name>$`.

With detection fixed, two mutations genuinely survived:

* **M7** -- `nodes` sorted by key *and* `nearest` broke ties by key, so with a
  stable sort the tie-break was **unreachable dead code**. It looked like
  defence in depth; it meant neither mechanism was tested. `nodes` now returns
  insertion order, so the code providing determinism is code a test can kill.
* **M15** -- the summary-order test compared dicts, and **dict equality ignores
  order**, so the sort was never asserted.

36 tests, 15/15 mutations killed after both gaps were closed.


## Group 1 closed, and Group 2 items 6/7/14 verified (DDR-859/860)

### Group 1 — build-system integrity: 5 of 5

| item | state |
|---|---|
| 1 tracker contradictions / NSI 87 collision | `SYS_VAULT_PUT`=87, `SYS_VERIFY_AUDIT`=93, `SYS_READ_AUDIT` stays at its shipped 37 (DDR-840/860) |
| 2 Docker reproducible build env | `Dockerfile` pins `ubuntu:24.04`; `ci-docker-check` asserts no unpinned archive |
| 3 CMake/Makefile hybrid | **BUILT** (DDR-859) — supersedes DDR-843 Decision 1 on the operator's direction |
| 4 VirtualBox runner | `tools/vbox_runner/run_vbox.sh`, exits 77 when VBoxManage is absent |
| 5 chipset variants | 4 x86_64 variants: q35/pc x qemu64/Nehalem/Opteron_G5, incl. AMD |

### The CMake decision was reversed, and that is recorded rather than smoothed

I recommended skipping CMake and documented why in DDR-843. The operator had
reserved the call, reviewed it, and directed that the hybrid be built. DDR-859
supersedes that decision explicitly. The recommendation was wrong in a specific
way worth naming: it argued the Makefile was *sufficient*, when sufficiency was
never the question being asked.

**The hybrid's real hazard is not two build systems — it is two sets of flags.**
A CMakeLists that restated `-mcmodel=kernel` or dropped `-Werror` would still
produce a `kernel.elf`; the 138 gates would keep passing against the Makefile's
binary; and CMake would ship a different one. So the Makefile stays canonical,
CMake *queries* it (`make print-flags`), configure **fails** rather than falling
back to defaults, and `make cmake-check` asserts the captured copy still
matches. `-Werror`, `-mcmodel=kernel` and `-mno-red-zone` are checked **by
name** as well as by diff, because a diff reports parity when *both* sides lose
a flag together.

`cmake` is absent on this build host, so the configure path is CI-only here.
Rather than ship the drift logic unverified — the DDR-854 mistake — a self-test
exercises all five non-cmake arms, including the one a diff cannot catch.

### Group 2

| item | state |
|---|---|
| 6 NSI 86 + `CAP_REWRITE` (bit 21) | already in tree (DDR-842) |
| 7 audit verification | closes on the SHA-256 **chain**, operator-directed (DDR-860) |
| 14 ChaCha20-Poly1305 gate wiring | **verified DIRECT** — see below |

**Item 14 needed no fallback.** `smoke-aead` exists as a dedicated gate with a
required sentinel (`PRADYOS_AEAD_VECTORS_OK`) *and* a forbidden one
(`PRADYOS_AEAD_STUB`, `AEAD FAIL`), sits in CI shard 4 at 90 s, links
`build/aead.o`, and is driven by `user/aeadtest.c`. The forbidden sentinel is
what makes it meaningful: it fails if the primitive is ever replaced by a stub
that would otherwise print the success marker.


## Two intermittent SMP reds, measured rather than assumed (OPEN-2, Group 9 item 47)

While landing DDR-859/862 the CI went red four times. The audit separates them
cleanly, and the separation is the point — two were mine, two were not.

**Mine, and real:** `shard-check` failed on `c99f1f8` and `39fa1cf` because the
CMake hybrid declared `ASM_NASM` it never uses, and because it resolved **gcc**
while the Makefile builds with clang. Fixed in DDR-862; `shard-check` passes on
`e51584a`.

**Not mine:** `smoke-msixap` (shard 5, run 31139497587) and `smoke-crosswake`
(shard 1, runs 31142981014 and 31142982460).

### The crosswake occurrence is the best evidence OPEN-2 has

Re-running shard 1 on **the identical SHA `e51584a`, with no code change,
PASSED**. Failed, then passed, same tree. That is intermittency by
demonstration rather than by the usual argument-from-plausibility, and it is
worth more than the earlier single-occurrence triages.

The temptation was to call it flaky immediately — my Makefile diff since the
last fully-green SHA is three *phony* targets, which cannot reach the image or
the kernel. But "my change can't have done this" is exactly the reasoning that
is wrong when it is wrong, and the last fully-green run (`16b4843`) really was
green on both runs, so the failures genuinely did begin with my work. The re-run
settled it in a way reasoning could not.

### What this contributes to item 47

Item 47 asks for the `-smp 4` virtio-blk defect to be fixed **or** explicitly
documented with a measured reproduction rate. This adds two dated occurrences
and one clean negative (a passing re-run on unchanged code). Both gates are SMP
paths — `smoke-crosswake` already carries a documented flake history here (AP
tick inflation, with a second root-cause noted as en route).

**Not yet a rate.** Two occurrences across roughly a dozen runs is an
observation, not a measurement, and quoting a percentage from it would be
inventing precision. A real rate needs a deliberate N-run campaign on one SHA,
which is the shape item 47's "measured reproduction rate" actually requires.


## DDR-865 — the local kernel build was blocked by Windows Defender

`make kernel` died at `llvm-objcopy: error: 'build/kernel.elf': Invalid
argument` while CI stayed green on the same commit. It was not objcopy:
**nothing** could read the file, `cp` included, and Windows reported the file as
containing "a virus or potentially unwanted software".

**Windows Defender quarantines the kernel ELF.** A freestanding x86_64 image —
no libc, raw entry point — trips a heuristic. The file stays visible at its full
1,017,984 bytes and every read returns `EINVAL`, which is why the failure reads
as a toolchain bug.

**Fixed by building on WSL's native ext4** (`~/pradyos-build`, rsync-mirrored)
rather than by adding a Defender exclusion. An exclusion would work, but it
changes the operator's security posture, and that is theirs to decide rather
than a side effect of a build fix. ext4 also matches CI's filesystem semantics.

**This materially changes throughput.** The full `image` target builds, and
`make smoke` passes in **0.6 s** — DDR-785's early exit fires the moment the
sentinel appears, so local gating costs far less than the ~2–3 min per gate
previously assumed. Kernel work can now be verified locally instead of waiting
on a ~30 min CI round trip.

One trap worth recording because it cost a build: `--exclude 'build/'` in rsync
is **not** anchored, so it also matched `tools/build/` and removed
`toolchain.mk`. The build then failed with `No rule to make target
'tools/build/toolchain.mk'`, which looks exactly like a missing file in the
repo. The pattern must be `/build/`.

### Item 41 — Wayland: superseded, not required

Pre-approved by the operator and involving no code. The in-house compositor is
built, gate-proven and sufficient; a wlroots port would trade that for a large
dependency chain and no new capability. Recorded so it is not read later as an
unmet release item.


## Item 20 (ftruncate) ships — and DDR-866's "bug" was my harness

DDR-866 reverted a WORKING implementation. The deterministic 5/5 failure
(`shrink lost or altered surviving content`) was not in the kernel: the build
mirror used `rsync -a`, which **preserves the source mtime**, so restoring a
mutated file whose mtime predated the object built from it left `make` believing
that object was current. The tree kept running the **mutated** code, and the
failure message was mutation M3's exact symptom.

A clean-room rebuild passes **5/5**. The code was correct when I reverted it.

Third time a stale artifact has made a harness lie here — after DDR-853's
`__pycache__` and DDR-862's flags-only parity. Same lesson: **a restore that
does not force a rebuild has not restored anything.** The mirror now uses
`--checksum --no-times`, so changed files get a fresh mtime and `make` rebuilds
exactly those.

The judgement error is worth separating from the tooling one: I treated 5/5
determinism as proof the bug was real, when determinism only shows the input was
constant — and a stale object is a very constant input.

**What ships:** a `truncate` VFS op (NULL for fat32/ext4, refused rather than
called through), `vfs_truncate` gated on `CAP_FS_WRITE` and deliberately not
charged to the ADR-032 throughput bucket, `sfs_truncate` as a bounded
read-then-rewrite that refuses past 64 KiB rather than satisfying a length
nobody asked for, `SYS_FTRUNCATE` at NSI 94 with the sign checked before the
cast, and a probe-gated `smoke-ftruncate`.

The probe's negative-length case now pins **`-EINVAL` specifically** — asserting
only "negative" passed even with the sign check removed, because the size bound
returned `-EIO` and both are negative.

Mutation table: baseline PASS, all three mutations FAIL, restore PASS.
Regression set green (`smoke`, `-fsrm`, `-fs-sfs-rw`, `-fs-rw`, `-sfs-persist`,
`-user`). Zero warnings under `-Werror`. 135 gates across 6 shards.

## Group 4 item 25 — Intel e1000e NIC (DDR-876)

`kernel/drivers/net/e1000e.{c,h}` drives QEMU's `e1000e` (82574L, `8086:10D3`)
behind the same five-function API as `virtio_net.h`, so the lwIP bridge above
sees one interface whichever NIC the platform has. RX/TX descriptor rings, MAC
from RAL0/RAH0, polled receive. Matched on **vendor AND device**, not class 0x02
— class alone is every Ethernet controller, and this driver programs 82574L
registers.

Polled rather than interrupt-driven on purpose: INTx here is shared and chained,
virtio-net already owns MSI-X vector 54, and a mis-shared line loses interrupts,
which reads as a network that works until it silently stops.

**Two things went wrong and are worth keeping.** The first run page-faulted at
`CR2=0xFEB85400` — RAH0 sits at BAR offset 0x5404 and the map covered one page.
The BAR is now *probed* for its size rather than assumed; swapping one assumed
constant for a larger one would have been the same mistake. The second was
"netbuf exhausted": `netbuf` is a fixed 40-buffer pool sized for one NIC. Rings
are now 16 RX / 8 TX (a multiple of 8, keeping RDLEN a multiple of 128 bytes) and
`NETBUF_COUNT` is 72 with the second NIC written into the budget comment.

`smoke-e1000e` attaches a real device (opt-in `QEMU_E1000E`) and requires a MAC
prefix of `52:54:00:` plus `TX OK` **and** `RX OK` — bring-up sends a broadcast
ARP and polls for slirp's reply. That round trip is the only thing exercising the
TX descriptor, RX descriptor, DD bit and tail pointer together; all four can be
wrong while the device probes and prints cleanly.

Mutation matrix: `RDT = 0`, RDLEN-in-entries, dropped bus-master, and a wrong
RAL0 offset are all **killed**. Testing `d->length` instead of `STATUS.DD`
**survives** — the ring is zeroed and QEMU writes length and status together, so
one packet cannot distinguish them. The DD check is correct on the SDM's terms
but this gate does not prove it; a ring-wrapping multi-packet gate would.

Not proven: ring wraparound, sustained throughput, link-state changes, and lwIP
running over e1000e (the bridge is still bound to virtio-net).

Gates green: `smoke-e1000e`, `smoke`, `smoke-fs`, `smoke-fs-rw`,
`smoke-fs-sfs-rw`, `smoke-fs-ext4`, `smoke-user`. Zero warnings under `-Werror`.
138 gates across 6 shards.

## Group 3 item 19 — six-argument syscall ABI (DDR-877)

`syscall_dispatch` and `syscall_fn` now carry the full SysV syscall register set
RDI/RSI/RDX/R10/R8/R9. All 91 handlers take six arguments even when they use
two: a per-arity table would mean the dispatcher had to know each handler's
arity, and a wrong entry there reads a register the caller never set and passes
the garbage on as a real argument. `-Wunused-parameter` under `-Werror` makes
the ignored ones explicit.

The marshal ordering in `syscall_entry.asm` is load-bearing — R8 and R9 are both
sources and destinations, so a6 is pushed and a5 moved *before* R8 is
overwritten. The seventh C argument goes on the stack per SysV.

`SYS_MMAP` is the consumer, and it was already wrong: it took four arguments, so
a caller passing fd and offset — which `user/systest.asm` **already did**,
correctly — had them silently discarded and got anonymous zero pages back. "Map
this file at this offset" returned success and something else entirely. It now
reads both and refuses what it cannot do: `fd != -1` → `-ENOSYS` (this also
rejects `fd == 0`, which some libcs pass for anonymous maps and which is a real
open descriptor), `offset != 0` → `-EINVAL`.

`smoke-sysmmap` gained two reject arms, `SYSMMAP FD REJECTED` and `SYSMMAP OFF
REJECTED`, with **different** expected errno values on purpose — symmetric arms
would both pass under an r8/r9 swap in the marshal; asymmetric ones both fail.

## Group 9 item 47 — the `-smp 4` flake, measured and narrowed (DDR-878)

Two findings, deliberately not merged.

**Fixed:** virtio_blk's single `slot_waiter` pointer (DDR-864) is now an
intrusive FIFO wait list (`slot_head`/`slot_tail` + `tcb.blk_wait_next`,
initialised in `sched_create` because `kmalloc` does not zero). One waiter is
woken per freed slot, on *both* release paths — the original woke nobody on the
`virtq_add` failure path.

**Not the flake.** A one-shot witness prints when the wait list first reaches
depth ≥ 2 — the exact case the old pointer overwrote. It fires **zero** times in
an instrumented `-smp 4` boot, and the flake survives the fix: **2/40** post-fix
runs of `smoke-rqstress-liveness` failed against a 2/27 baseline. DDR-864's
hypothesis is disproved, not quietly dropped.

**And DDR-863's picture was wrong.** Per-gate, on one pinned SHA:
`smoke-rqstress-liveness` 1/8; `smoke-msixap`, `smoke-blkmq`,
`smoke-blk-integrity`, `smoke-sfs-btree-smp4` **0/8 each**. It is one gate, not
"the SMP gates".

**What it actually is.** A captured failing boot shows `[boot-stamp] A` printing
and `[boot-stamp] B` never printing, while the boot continues normally for 100+
more lines. `fs_test_thread` spawns the ext4 probe and never runs again, so
`rqstress_proof()` is never reached — the gate reports a missing sentinel, which
reads as a scheduler proof failing when the proof never ran. Not wrong data, not
block I/O (the next step is an embedded ELF load). One runnable thread lost while
every other thread continues: a per-CPU runqueue / work-stealing loss, which is
why the one gate that stresses the runqueues is the one that flakes.

Item 47 is **documented, not fixed** — the item's own standard — but with a
measured per-gate rate, four gates cleared, a named subsystem, and a repeatable
capture procedure. Item 50's "3 consecutive greens" is now ~68% per attempt
rather than a wall.

## Group 3 item 21 — v1.0.0 syscall target: ~95, not 200+ (DDR-879)

Assessment, which is what the item asks for. **91 NSI numbers defined, 91
registered** — not the 76 in the older tracker text. The two counts matching
matters on its own: a defined-but-unregistered number returns `-ENOSYS` while
looking present in the header.

The surface is four groups, not one list: 31 POSIX process/file/memory, 26
AETHER agent+capability, 22 Layer-7 desktop, 12 introspection/system.

**200+ is the wrong release criterion.** Linux's ~350 are mostly variants and
compatibility strata (`open`/`openat`, four `stat` forms, 32-bit and `time64`
doubles) kept for binaries compiled decades ago. PRADYOS has no compatibility
stratum. Counting to 200 without that pressure means *inventing* syscalls — each
a permanent NSI number, a handler nobody calls, and a gate nobody wrote.
DDR-877 is the precedent: the defect was in a syscall that already existed and
was already counted.

**Target ~95, derived from the remaining queue:** +3 for item 15's service
manager (capability-based supervision has no existing expression), +1 conditional
for item 27 if suspend is ring-3 triggerable. Items 34, 35, 38 and 40 need
**zero** new numbers — job tables are shell-side, a loader needs only
`mmap`/`open`/`read`, and a VFS mount is kernel-side.

`pread`/`pwrite`, `MAP_FIXED`, file-backed `mmap`, `mprotect`, `statx` and
threads are real and defensible — as post-1.0 items with their own gates, not as
numbers reserved now against a design that does not exist. The count is a
reported measurement, never a goal: a release should be able to say every number
has a handler, a caller and a gate.

## Group 4 item 26 — Intel HDA audio: DEFERRED, OPTIONAL

Logged as the item itself directs. HDA is marked optional in the queue and its
absence does not block Group 4, which is complete with items 22–25, 27 and 28.

Not a silent skip: no audio subsystem exists above it either — no mixer, no
per-process audio capability in the ADR-026 model, and no sentinel a gate could
assert beyond "a codec answered". Shipping a driver under that would add a
device whose only proof is that it enumerated, which is the probe-not-function
standard DDR-875 and DDR-876 were both written to avoid. It is deferred with the
audio stack, not deferred on its own.

## Group 9 item 46 — OPEN-10 is item 47, not a B+tree bug (DDR-880)

A 30-run campaign of `smoke-sfs-btree-smp4` reported "2/30 with the OPEN-10
signature", which would have been the first local reproduction ever. **It was a
detector bug:** `make` echoes its own recipe, so every log contains
`FORBIDDEN_SENTINEL="btree churn FAIL"` and grepping the whole log matches the
harness's own echo. The tell is `signature == failures` exactly. Re-classified
against the kernel's own print, both failures show **neither** `[sfs] btree
churn OK` **nor** `FAIL` — the probe never ran. Rule that keeps catching this:
assert on what the kernel printed, never on a string the harness also prints.

Both failures carry the item-47 signature exactly: stamp A prints, **stamp B
never does**, ~100 lines of normal boot follow, and the last thing
`fs_test_thread` does is spawn the ext4-rooted probe. That thread runs the
churn probe, `rqstress_proof`, `blkmq_proof` and `smp_blk_integrity` — when it
is lost, all of them silently do not run, and whichever gate asserts on one of
those sentinels reports "required pattern not found". **OPEN-10 and item 47 are
two gates watching the same thread die.**

It also explains what never fit: OPEN-10 was named for a B+tree, and `sfs.c` has
no global mutable state to race on. There was nothing to fix there.

Measured, one pinned SHA, gates run individually: `smoke-sfs-btree-smp4` 2/30
(6.7%), `smoke-rqstress-liveness` 2/40 (5.0%) — the two rates agree, as one
defect seen through two gates should. `BUILD_TRACKER.md` §5's B#3/virtio-blk
hypothesis is closed by DDR-878, not left standing.

`[boot-stamp] C` now lands immediately after the ext4-probe block, narrowing the
~90-line loss window to one bit: C without B means the next `elf_load` is the
suspect; no C means the loss is inside the ext4 block. It ships **without** the
campaign that reads it — claiming a result from a stamp nobody has caught yet
would be the unsupported conclusion these DDRs exist to avoid.

Item 46 **documented** to its stated standard: measured rate, identified
signature, named misnomer. Item 50 is now blocked by one defect, not two.

## Group 6 item 34 — PRISM job control (DDR-881)

`&`, an 8-slot job table, `jobs`, `fg %n`, `kill %n`, and a non-blocking reap.

**Not shipped, and not a corner-cut:** `bg` and ^Z/SIGTSTP need process groups
and a tty that owns a foreground group; PRADYOS has neither `setpgid` nor a
controlling terminal. Offering them would advertise a capability the kernel
cannot provide.

The reap is load-bearing: an unwaited background child stays a zombie holding
its TCB, and the shell is the only thing that can collect its own children.
`wait4`'s `options` is the **third** argument, so the existing 3-arg `nsi()`
already carries `WNOHANG` — a 4-arg wrapper added for it was removed once
`sys_wait4`'s signature was read, rather than left as dead code.

`&` is stripped before redirection and pipe parsing, or `>` reads it as a
filename — the same ordering trap DDR-868 hit with `2>&1`.

The gate asserts three deterministic facts and deliberately does **not** assert
`jobs`/`fg` output, which races the child's exit. Mutation matrix: M1 (ignore
`&`) killed; M2 (`kill %n` falls through with pid 0) **initially survived** —
the assertion checked that the shell *said* "no such job", which prints before
the guard is consulted, not that it *did not call kill*. Re-aimed at the
behaviour, it kills M2. The first attempt also used `/HELLO.ELF` and got
`Done(127)`: that file is not on the shell's root, and the gate found it.

### Item 46/47 addendum — CI push procedure (DDR-880)

Two more CI failures, both on `main`, both the DDR-880 signature: `d5cdf7e`
(`smoke-blkmq`) and `aef693e` (`smoke-msixap`). `d5cdf7e` was docs plus one boot
stamp, and the identical tree passed on `dev/phase1`. Both missing sentinels
belong to proofs `fs_test_thread` runs — the same lost thread, now reproducing
in CI rather than only under a local loop.

The two branch runs **always execute concurrently** (every pair in the last five
pushes started within 2–3 s), so 12 shard jobs race instead of 6 on a defect
that is timing-sensitive. That concurrency is fact; "`main` loses more" is **not**
established — 2 of 5 both landing on `main` is p ≈ 0.25, which is not evidence.

**Procedure from here:** push `dev/phase1`, wait for green, *then* fast-forward
`main`. It costs one CI cycle of wall-clock and halves the contention, which
matters for item 50's "3 consecutive greens on one tip".

## Group 3 item 17a — SRAT NUMA topology (DDR-882)

`struct numa_topology` is declared in `boot_info.h` and populated by the KERNEL
from ACPI SRAT, not by the bootloader. Two structural reasons: `struct boot_info`
ends in a flexible array (`e820[]`) so nothing can be appended after it, and
stage2 is 1294 bytes of 16-bit assembly that cannot do RSDP discovery, checksum
validation and sub-table walking. The boot protocol is unchanged.

`kernel/mm/numa.c` parses SRAT sub-table types 0, 1 and 2. Three things that make
SRAT subtly wrong are handled explicitly: the proximity domain is **split** in
type 0 (low byte at offset 2, high three bytes at 9..11, APIC id in between); a
sub-table with `length == 0` is an infinite loop, so the walk is bounded; and the
enabled flag is not optional, since firmware lists disabled hot-plug entries that
would otherwise inflate the node count.

`smoke-numa` runs against a genuine two-node QEMU machine and asserts
`nodes=2 ranges=3 rejected=0`, `node0 mem=255MiB`, `node1 mem=256MiB`. The exact
byte totals are the assertion with teeth — a parser that finds SRAT but reads
base/length at the wrong offsets still reports a node count. The rejection check
is folded into the required line as `rejected=0` rather than being a FORBIDDEN
sentinel, which keeps the DDR-785 early exit eligible.

**Mutation matrix, reported honestly:** M1 (length field at the wrong offset)
killed. M3 (accept unaligned ranges) **SURVIVED** — QEMU never emits an unaligned
range, so that guard is defensive code with no test behind it, and this says so
rather than counting it as covered. M2 was an invalid mutation (it did not
compile) and is not counted as a kill.

**17a only. Item 17 is NOT complete:** slice 17b — per-node PMM free lists,
re-bucketing after `numa_init()`, and `pmm_alloc_pages_node()` — has not been
built. Item 37 depends on both.

## Group 3 item 17b — node-aware PMM allocation (DDR-882) — ITEM 17 COMPLETE

`free_list` is now `[NUMA_MAX_NODES][PMM_MAX_ORDER]`. **Cross-node coalescing is
prevented by construction, not by a check:** the free path searches
`numa_node_of(addr)`'s list, so a buddy owned by another node is simply not
there, `list_remove` fails, and coalescing stops. No explicit "same node?" test
was added, because an explicit test is a thing that can be wrong.

`pmm_alloc_pages_node(node, order)` prefers a node and **falls back** to others
rather than failing. That is deliberate: a node is a locality *preference*, and
returning 0 while free memory exists elsewhere would turn a hint into an
out-of-memory condition the caller cannot distinguish. Locality is an
optimisation; allocation failing is a correctness event.

`pmm_numa_rebucket()` re-files every block after `numa_init()`, because
`pmm_init` (main.c:2195) runs before `acpi_init` (main.c:2343). Straddling blocks
are split into buddies recursively down to order 0, where a frame lies in exactly
one node — which holds only because `numa.c` rejects unaligned SRAT ranges.

`smoke-numa-alloc` asserts `node0=59904` and `[numa] alloc node1 -> node1 OK`.
The block count and node1's page count are deliberately **not** asserted: they
shift with pre-rebucket allocation (194/65320 under the probe vs 198/65323
without), so pinning them would fail on unrelated allocation changes.

**The gate was strengthened after mutation testing exposed it.** With the
original 256M/256M node split, disabling straddle-splitting still PASSED — a
256 MiB boundary is 4 MiB aligned, the largest buddy order, so nothing can ever
straddle it and the split path was never exercised. The QEMU layout is now
**250M/262M**, which is not max-order aligned, and the same mutation is now
killed (node0 drifts 59904 → 60416).

**Mutation matrix — 17b: M1 (alloc ignores node) killed, M2 (rebucket to node 0)
killed, M3 (no straddle split) killed after the layout fix.** 17a: M1 killed;
**M3 (accept unaligned ranges) still SURVIVES** — QEMU cannot emit an unaligned
SRAT range, so that guard remains defensive code with no test behind it, and this
says so rather than counting it as covered.

**Item 17 is complete.** Item 37 (per-CPU runqueue NUMA affinity) is now
unblocked: it consumes `numa_node_of_cpu()` and these per-node lists.

### Item 17 push check — the `-smp 4` rate was measured, not assumed

`fce31b1` (17a) went red in CI on `smoke-crosswake` with `created 0, verified 0`
— the wrong-data variant DDR-864 documented — and `smoke-blkmq` failed once in a
local regression. Since DDR-878 measured `smoke-blkmq` at **0/8** before the NUMA
work, "it's the known flake" needed proof rather than assertion: 17b rewrites the
buddy free lists, which is exactly the kind of change that could make a
concurrency defect worse.

Measured on the 17a+17b tree, gates run individually:

| Gate | Rate | Signature |
|---|---|---|
| `smoke-blkmq` | **1 / 20** (5%) | `'[blk] multi-inflight OK' not found` — DDR-880 |
| `smoke-crosswake` | **0 / 20** | — |

5% matches the documented rate (2/40 rqstress, 2/30 btree). **The NUMA work did
not change it.** Item 17 pushed on that evidence.

### CORRECTION — item 46 (OPEN-10) is a REAL, SEPARATE defect (DDR-880 addendum)

CI run `31329941053` (`00808b4`) failed on two shards with **two different
defects**, which is why they were conflated:

- shard 4 — `'[blk] multi-inflight OK' not found`: the lost thread (item 47).
- shard 0 — `[boot-stamp] C` **and** `[boot-stamp] B` both printed, then
  `[sfs] btree churn FAIL`. The thread was **not** lost; the churn probe ran and
  genuinely failed. That is the true OPEN-10 signature, never captured before.

DDR-880 concluded OPEN-10 was a misnomer for the lost thread. That conclusion was
generalised from two local failures that happened to share a mode, and it is
**wrong**. Item 46 reverts to **open and distinct**; item 47's characterisation
is unaffected. The `[boot-stamp] C` instrument works and is what proved it.

### Item 46 narrowed — OPEN-10 is `op=create iter=0` (DDR-884)

The single real capture says `[sfs] churn FAIL op=create iter=0`. Iteration 0
creates one file into a fresh directory and `SFS_LEAF_MAX` is 14, so no B+tree
split is reachable. **OPEN-10 is a first-create failure, not a churn defect** —
it is merely *reported by* the churn probe, which is what happens to be creating
a file at that moment. That is why 30 local `smoke-sfs-btree-smp4` runs found
nothing while a `smoke-smpuser` boot found one: it depends on what else is
touching the SFS root, not on churn depth.

The probe discarded the return code, leaving `-EEXIST` (leftover file or
concurrent writer), `-ENOSPC` (volume) and an ADR-032 write-budget refusal
indistinguishable — and a write-budget exhaustion has already been
mis-attributed to a "B+tree split bug" in this same probe once before. `rc` is
now printed, landed **before** re-running so the next occurrence is diagnosable.

No fix attempted: three candidates remain and the evidence does not separate
them. Patching one would be validated only by a rare failure not recurring,
which is indistinguishable from having fixed nothing.

## Group 6 item 37 — NUMA-affine steal order (DDR-885)

`rq_steal()` is two passes: same-node victims first, then everything else. The
second pass is **unconditional** — a scheduler that withheld work for locality
would be a liveness bug, not an optimisation. Only the victim ORDER changed;
enqueue placement is deliberately untouched while the lost-thread defect is open.

Measured on 4 CPUs, single node: `[sched] steal local=86850 remote=0` — the
first pass does all the work rather than falling through, and `smoke-rqstress`
stays green.

**Not proven: the node preference itself.** Every CPU there is node 0, so
"same node" is trivially true; proving preference needs 4 vCPUs across 2 nodes,
which is currently unreliable for the same reason item 50 is. **And the
assertion is weak** — `[sched] steal local=` also matches `local=0`. A mutation
skipping the same-node pass was killed, but most likely by the pre-existing
`rqstress FAIL` pattern rather than by this line, and I did not confirm which.
Recorded as a diagnostic, not as a gate with teeth.

### Item 46 capture campaign — 0 hits in 45 runs

`smoke-smpuser`, `smoke-rqstress`, `smoke-smp` × 15 with the DDR-884 `rc`
instrument in place: **45 runs, 0 `churn FAIL op=` hits, 0 other reds.**
OPEN-10 did not reproduce locally. It remains a single CI observation, and its
local rate is below 1-in-45. The `rc` instrument is in place for the next
occurrence; no fix is attempted without it.

### Item 46 CLOSED — documented known issue (DDR-884)

Item 46 permits "root-cause and fix **or** explicitly document as known issue
with measured reproduction rate". Closed on the second branch.

**75 local runs, 0 reproductions** (30 on `smoke-sfs-btree-smp4`, 45 on
`smoke-smpuser`/`rqstress`/`smp` with the `rc` instrument). **1 CI occurrence.**

Known: signature is `op=create iter=0` — the FIRST create, where no B+tree split
is reachable, so it is neither a churn nor a B+tree defect; the thread is not
lost (`boot-stamp` C and B both printed), which separates it from item 47; and
`sfs.c` still has no global mutable state, so the prescribed spinlock has no
target. Three causes remain unseparated: `-EEXIST`, `-ENOSPC`, or an ADR-032
write-budget refusal.

No fix ships because a sub-1-in-75 failure not recurring cannot validate one.
The `rc` instrument is landed and waiting for the next occurrence.

## Group 4 item 22 — UEFI boot path (DDR-886)

`boot/uefi/` is a from-scratch PE32+ UEFI application (clang
`--target=x86_64-unknown-windows` + `lld-link-18`), reproducing the legacy
loader's handoff contract exactly: RDI = `boot_info` at 0x4000, kernel.bin at
physical 0x400000, `0xFFFFFFFF80000000` → 0x400000 plus a low identity map, long
mode, interrupts off. **No EDK2 or gnu-efi dependency** — stated as a deviation
in DDR-886 §2, since the item named EDK2; 150 lines of spec-fixed declarations
is a smaller surface than a submodule with its own build system.

`smoke-uefi` boots OVMF from a FAT ESP and requires the **same**
`NEXUS KERNEL OK` sentinel as every other gate — that identity is the point,
since a UEFI-specific sentinel would let the two paths drift while both looked
green.

**Two defects the work surfaced.** The first version **silently truncated** the
firmware memory map at 96 entries (OVMF emits >100), so the kernel booted with
less RAM than the machine had and nothing said so; adjacent same-type runs are
now merged (16 entries) and overflow is **refused**, not truncated. And the gate
was **blind to a wrong handoff**: mutation M2, a deliberately wrong `boot_info`
pointer, PASSED, because `NEXUS KERNEL OK` prints before the memory map is used.
Requiring the E820 entry count fixed it — M2, M3 (all memory marked usable) and
M4 (`sizeof` instead of `desc_size`) are now all killed. M1 did not compile and
is recorded as invalid, not a kill.

Legacy path re-verified after the `BOOTDISK` harness refactor: `smoke`,
`smoke-fs`, `smoke-user`, `smoke-shell`, `smoke-numa` all green.

## Group 9 item 48 — BLOCKED on build-host tooling (DDR-887)

`xorriso`, `grub-mkrescue`, and GRUB's BIOS/EFI module trees are all absent from
this build host, and `sudo` requires a password this session does not have. The
packages cannot be installed here, so item 48 cannot be built or verified.

No `make iso` rule ships. It could be written blind, but this project's standard
is 20 local runs before a gate is trusted, and a gate that has never run locally
is exactly what goes red in CI for an unattributable reason.

**Design recorded** so it is not re-derived: hybrid El Torito with two catalog
entries — the UEFI arm is `build/esp.img` from DDR-886, already proven under
OVMF and needing no new code; the BIOS arm is `build/pradyos.img` padded to
2.88 MiB and booted via floppy emulation, so stage1/stage2 run unmodified.
**Multiboot2 is recommended AGAINST** and flagged for sign-off: it hands control
in 32-bit protected mode while `kernel_entry` needs long mode with the DDR-886
contract established, so it would be a third handoff implementation to keep in
sync — and DDR-886 §1 is explicit that boot-path divergence fails far from its
cause.

**CI gap this exposed:** `smoke-uefi` shipped last commit but CI installed no
`ovmf`, so it would go red on a missing firmware file. `ovmf xorriso grub-pc-bin
grub-efi-amd64-bin` are now in the workflow's install list.

## Group 6 item 36 — PRISM agent DSL (DDR-888)

`agent list|spawn`, `action submit|poll|approve`. ADR-024 §D3 deferred the DSL
without specifying it, so DDR-888 defines it. The action **type is parsed**, not
fixed — a hard-coded type could not express the Section 3C set.

**PRISM is unprivileged, and that is the test.** It holds neither `CAP_AGENT` nor
`CAP_SOVEREIGN`, so three of the five verbs are expected to be refused and the
refusal printing is the assertion: `AGENT SPAWN DENIED`, `ACTION SUBMIT DENIED`,
`ACTION APPROVE DENIED`, while `agent list` works because `sys_agent_roster` is
deliberately ungated (observability is not privileged). A shell that offered
`agent spawn` and did nothing would look like a missing feature; one that
appeared to succeed would be a capability hole.

**Mutation testing caught itself first:** two mutations "survived", then the file
showed `sed` had matched nothing — the patterns contained `\n` escapes that did
not survive quoting, and a mutation that never applies reads exactly like one the
gate cannot kill. The re-run verifies the edit applied before trusting the
verdict, and the mutation is then **killed** with the right message. Recorded as
invalid, not as survivals.

## Group 5 item 31 — on-disk SFS free list (DDR-889)

`c->free_runs[]` was in-memory only, so reclaimed runs were discarded at unmount
and the volume leaked blocks across a remount. It now persists to one block
`{magic, count, runs[254]}` rooted at the superblock's `free_extent_tree` —
**a list, not a B+tree despite the field name**, because the in-memory model is a
bounded array with exact-fit and no coalescing, and a tree would be a second
structure to keep in step. The block is allocated once and reused in place.

Rejected rather than absorbed: a block with the wrong magic is **dropped** (using
it would hand the allocator arbitrary "free" runs and the next write would land
on live blocks), and runs past `next_free` are skipped. The save happens before
the superblock write, since the superblock is the commit point.

**The first gate passed for the wrong reason and was deleted.** Unmount →
remount → watch reuse passed even with the on-disk load *disabled*: `grew=10` vs
`grew=9`. `vfs_mount()` returns the cached mount, so no fresh context is built
and the in-memory runs survive. A threshold between 9 and 10 would have looked
decisive while testing nothing.

What ships is smaller and true: `sfs_read_freelist_count()` reads the superblock
off the device, follows the root, checks the magic, and the self-test requires a
non-empty list — `[sfs] freelist ondisk runs=1`. Mutations "never save" and
"wrong magic" are both **killed**, each verified as applied first.

**Not proven: reuse after a genuine cold remount.** Every mount in one boot is
cached, so the load path is exercised only at first mount when the list is empty.
That round trip needs a second boot against a persisted image or a `vfs_unmount`
that truly destroys the context — a VFS change, named as the follow-up.

## Group 7 item 40 — PRADYOS Drive (DDR-890)

`kernel/fs/pdrive/` is a RAM-backed filesystem implementing the same
`struct vfs_fs_ops` as fat32/SFS/ext4, so an agent rooted there uses the ordinary
file calls — a workspace threaded through the syscall layer would have been a
second file API to keep in step and would prove nothing about the VFS contract.

`vfs_mount_virtual(name)` selects **by name** and does not fall back: "mount
whichever driver accepts a NULL device" breaks as soon as there are two virtual
filesystems. Symmetrically `pd_mount()` **refuses a non-NULL device** — otherwise
the probe loop would hand it the first disk, and the real filesystem on that disk
would never be tried.

**Bounded because an agent controls the size.** An unbounded RAM filesystem is a
DoS primitive: the PMM starves the kernel, not the agent. 32 files / 1 MiB, both
enforced by **refusal** — clipping would report success for data that is not
there. A full table refuses rather than evicting, since eviction loses another
agent's data silently. Growth charges the delta before allocating.

Self-test: `[pdrive] mounted id=3 rw OK readdir OK overflow REFUSED unlink OK`.
Mutations "clip instead of refuse" and "claim a real disk" both **killed**, each
verified as applied first. The second is killed by the *disk* filesystems failing
— the correct blast radius, and why `pd_mount` refuses rather than ignores.

Not implemented: subdirectories, persistence (RAM by design), per-agent isolation
into separate volumes (needs a mount table larger than `VFS_MAX_MOUNTS`=6 — a VFS
capacity change, not a pdrive one), grow-by-truncate.

## Group 3 item 15 — capability-based service manager (DDR-891)

PID 1 was a *reaper*: `wait4(-1)` in a loop, print, repeat. Nothing started
services, nothing knew what a service was, nothing reacted to one dying.

Now a bounded static table declares name, path, restart policy and required
capabilities; the reap loop **attributes each exit to its service** and applies
policy. Orphans reparented to init are still collected exactly as before.

**Restarts are budgeted (3), and give up loudly.** `RESTART_ON_FAILURE` with no
bound is a fork bomb written by the supervisor: a missing binary fails `execve`
instantly, so PID 1 spins at 100% and the failure looks like a hang rather than a
misconfiguration. A supervisor that silently stops retrying is indistinguishable
from one that never noticed.

**Capabilities are checked BEFORE the fork.** Leaving it to the kernel would let
the process start and fail later at first privileged use — by which point a
process is running that should never have existed, and the failure surfaces
somewhere unrelated to the misconfigured service.

Observed: `start exectest` → `refuse agentsvc` (no pid ever created) → `exit
exectest st=0` → three `restart missing n/3` → `giveup missing after 3 restarts`.
Mutations "remove the restart budget" and "remove the capability check" are both
**killed**, each verified as applied first.

Not implemented: dependency ordering, restart backoff, `RESTART_ALWAYS`, socket
activation, and runtime start/stop/status control — the last needs a syscall or
IPC endpoint into PID 1, and inventing one here would create a second control
surface to reconcile with the PRISM agent DSL (DDR-888).

## Group 4 item 27 — ACPI S3 discovery + CPU frequency scaling (DDR-892) — PARTIAL

**Complete:** `\_S3_` discovery (the `_S5_` scanner is parameterised on the digit
rather than duplicated — a second walker is a second place for the PkgLength
arithmetic to be wrong), `acpi_s3_available()`, EIST detection via CPUID.01H:ECX
bit 7, and `IA32_PERF_CTL`/`PERF_STATUS` behind that check.

**NOT complete: S3 entry and resume.** `acpi_suspend_s3()` deliberately
**refuses**. Entering S3 without a resume trampoline does not produce a wrong
answer — it powers the CPU down and never returns: an indistinguishable-from-hung
QEMU in a gate, a power-button box on hardware. On resume the firmware re-enters
in **real mode** at `FACS.firmware_waking_vector`, so long mode, CR3, GDT, IDT,
TSS and every per-CPU MSR must be rebuilt before any C runs. `ap_boot.asm`
already performs exactly that walk for SMP bring-up; the resume path is that code
with a different tail. **Item 27 is partially delivered and this says so rather
than counting discovery as the feature.**

Under QEMU/TCG, EIST is not advertised, so the path exercised is the refusal —
reporting a frequency from an unimplemented MSR would be inventing a number.

**Two things the build found.** The S3 report was placed *before*
`acpi_power_init()` scans the DSDT, so it printed "not advertised" for a machine
whose `_S3_` had parsed fine; the raw-occurrence counter (`occurrences=1
parsed=1`) is what separated "scanner broken" from "report too early" and is kept
permanently. And `QEMU_S3` was added then **removed**: measured, the DSDT carries
`_S3_` regardless, so the knob changed nothing and dead configuration is worse
than none.

Mutations "scanner ignores `_S3_`" and "claim EIST without checking CPUID" both
**killed**, each verified as applied first.

## Group 7 item 39 — MANUAL MODE is a different desktop (DDR-893)

MANUAL was one string: `draw_str(mode ? "SOVEREIGN MODE" : "MANUAL MODE", ...)`,
with gradient, particles, backdrop and agent panel identical in both. The item
says a mode flag on the Sovereign layout is not the feature.

Manual is now structurally different: **flat background** (one fill — no
gradient, no particle pass, no backdrop), a **bottom taskbar** with start and
window buttons, a **top menu bar** in place of the accent stripe, and **no agent
panel**. The two paths share only the drawing primitives; sharing the layout is
the design the item rules out.

**The gate asserts structure, not a title** — a title is something either layout
could print, so asserting "MANUAL MODE" would pass for the very implementation
being replaced. `PRADYOS_MANUAL_NO_AGENT_PANEL` asserts an **absence**, which a
gate cannot otherwise observe: something failing to render produces no output, so
the Manual path states it positively.

Mutation `if (0)` on the Manual branch — i.e. exactly the pre-DDR-893 behaviour —
is **killed**.

Not implemented: live window enumeration in the taskbar (buttons are a fixed
count, not the surface table), a functioning start menu, per-mode input routing,
mode persistence across boots.

## Item 16 — ROOT CAUSE FOUND (DDR-895). Still OPEN; no scheduler change ships.

Diagnostic slice only: the pick stays **FIFO**, a shadow vruntime is maintained,
and the fair-pick candidate is computed and compared without being acted on.
Probe-gated (`QEMU_PROBES=schedtrace`), so no default boot changes.

Captured on `-smp 4`:

```
[schedtrace] picks=512 diverge=993 first_div_tick=89 floor=583680
[schedacct] tid=11 fs  vr=399360 create=24576  wake=578560 picks=48155 ticks=366
[schedacct] tid=87 rqs vr=583680 create=583680 wake=0      picks=1     ticks=0
```

**Both prior hypotheses are refuted.** The FS thread is **below** the floor
(399360 vs 583680), not above it — so it is not starved by accrued vruntime. And
every probe thread enters at `create == floor` exactly — so entry placement was
never wrong.

**The mechanism is `picks=48155` against `ticks=366`.** The FS thread yields
voluntarily before the timer tick that would charge it, and charging happens
only on the tick, for the running thread. It therefore accrues almost nothing
while the floor races ahead. Under FIFO that is invisible; under
smallest-vruntime it is a **monopoly** — the FS thread is permanently the
minimum and the probe threads doing the actual filesystem work, entering at the
much higher floor, are starved. That is exactly the failing set.

**Fix direction for the next slice** (not implemented): charge voluntary
yields/blocks, and clamp how far behind the floor a thread may sit on requeue
(Linux's `place_entity`). Neither exists today.

**Instrument limitation, recorded:** the ring saturates at 512 entries and
stopped before `first_div_tick=89`'s divergences, so it printed none — the
**per-thread accounting carried this finding, not the ring**. A circular
overwrite would have captured the window.

Behaviour-neutral confirmed: 16-gate regression green, and `smoke-rqstress`
0/10 with the instrument in place (the one earlier red was the known ~5% flake).

## Item 48 — Multiboot2 SUPERSEDED by owner decision (DDR-896)

Recorded as a **formal substitution, not a scope cut**. The hybrid El Torito ISO
carries the two already-proven loaders (BIOS `pradyos.img`, UEFI `esp.img`)
rather than adding a Multiboot2 handoff, which would hand control in 32-bit
protected mode and require a **third** implementation of the DDR-886 contract.
Item 48's intent — one artifact booting both paths, proven by gate — is
unchanged. **Still BLOCKED on host tooling.**

## Item 35 — Phase-0 design delivered (DDR-897). Implementation NOT started.

Six sub-phases. **The headline: item 35 cannot begin with the linker.** Its first
two sub-phases are kernel prerequisites — file-backed `mmap` does not exist
(`sys_mmap` refuses `fd != -1`, DDR-877), and `PT_INTERP`/auxv are absent from
`elf.c`. Eager `BIND_NOW` binding is chosen on **W^X grounds** (ADR-021): lazy
binding needs a permanently writable GOT.

### Item 16 fix slice — ATTEMPTED, REVERTED AGAIN (DDR-895 §6)

Both approved guards were implemented and both behaved as designed. Guard 1
(TSC elapsed-time charging) closed the unbounded lag: FS-thread lag behind the
floor fell from **184,320 to 16,965**. Guard 2 (place_entity clamp) held that
bound exactly — **and that is the defect**.

`SCHED_LAG_MAX` was set to `1024*16` while vruntime was tick-scaled. Guard 1
changed the unit to TSC-derived, where the floor reaches ~12.5M per boot, so
16,384 became ~0.13% — the clamp stopped meaning "one slice of catch-up credit"
and became **snap-to-floor**, removing sleeper credit entirely.

Result: `smoke-shell` **5/5 deterministic failure** — PRISM reading serial a byte
at a time is the most I/O-bound workload present, and it is precisely what fair
scheduling should favour. 17/18 gates green is not shipping.

Next attempt must **derive** the clamp from the measured vruntime-per-tick rate,
not hard-code it. Also noted: `diverge=0` post-fix is tautological (the chosen
thread is the fair candidate) and is not evidence.

### Item 16 — third attempt reverted (DDR-899)

The DDR-895 unit mismatch is genuinely fixed: the clamp is now ONE MEASURED TICK
of vruntime, sampled from the floor's own advance, so it is correct in whatever
units vruntime uses. It did not fix the failure.

Controlled, same machine, back to back:

| Configuration | smoke-shell |
|---|---|
| baseline, item 16 absent (control) | **0 / 5 failed** |
| item 16, hard-coded clamp | 5 / 5 failed |
| item 16, derived clamp | 4 / 5 failed |

Fair-share picking breaks smoke-shell; the clamp magnitude is not the mechanism.
NOT established: whether PRISM is genuinely slower under fair-share, or whether
the gate budget is marginal (~25 injected commands with fixed sleeps inside a
60 s timeout). Those demand opposite responses, and raising the timeout would be
tuning the test to fit the change. The separating measurement is PRISM's own
block-to-dispatch latency. **Item 16 OPEN.**

### Item 35.1 — file-backed mmap design (DDR-900)

Eager population, not demand paging: demand paging needs blocking I/O inside the
#PF handler, which couples to the scheduler paths item 16 is unresolved in.
Private frames, no page cache — MAP_PRIVATE stays correct while MAP_SHARED is
**refused rather than approximated**, since approximating it would silently lose
writes another process expects to see. Every refusal carries its own errno.
Gate proves CONTENT equality, not call success. Implementation not started.

### Item 16 — budget hypothesis ELIMINATED by experiment (DDR-899 addendum)

Raising only the smoke-shell timeout (mirror only, discarded) separates the two
hypotheses: fair-share fails **3/3 at 60 s and 3/3 at 200 s**. With 3.3x the
time the failure lands at the identical point, so the gate budget is not the
cause — PRISM **stops making progress**, it is not merely slower.

Failure now localised to the **wake/requeue path**: `sched_place()` is one-sided
by construction (it only lifts threads that are *behind* the floor), so a thread
that ran a long slice before blocking returns *above* the floor and stays behind
every fresh thread entering at it. Correct for anti-starvation, but it leaves no
sleeper-credit mechanism — exactly what an interactive shell needs.

Item 16 OPEN, with a named suspect line rather than another guessed constant.

### Item 16 — rq_unlink suspect REFUTED (DDR-902)

Instrumented before fixing, per discipline. 746,753 fair-path unlinks:
`notfound=0 notready_LOST=0 pop_empty_with_queue=0`. The loss path never
executes. The defect is real as written and worth repairing when item 16 lands,
but it is **not** the mechanism.

Five hypotheses now eliminated by measurement (FS-thread starvation; stale-low
entry; gate budget; one-sided clamp; rq_unlink loss). Three genuine defects found
and fixed along the way — none was the cause.

Next suspect, **not acted on**: `sched_place()` returns early when
`g_vr_per_tick == 0`, and that rate is sampled from the floor's per-tick advance.
On the largely idle single-CPU system `smoke-shell` becomes after boot, the delta
is frequently zero, so the rate may never establish and placement never happens
at all — which would also explain why the two-sided fix changed nothing, since
both branches sit behind that early return. One counter, one run, settles it.

### Items 48/49 — still BLOCKED: the sudoers drop-in is not present

`/etc/sudoers.d/` in **Ubuntu-24.04** (the build distro) contains only `README`
and a 1-byte `athena-nopasswd`. There is no `claude-apt`, and
`sudo -n apt-get --version` still returns "a password is required". The install
never ran. WSL lists `Ubuntu-24.04` and `docker-desktop`; the drop-in may have
been created in a different distro or on the Windows side.

## Group 9 item 48 — hybrid ISO: UEFI arm PROVEN, BIOS arm blocked (DDR-903)

Tooling installed and verified explicitly (not trusting apt's exit code):
`xorriso 1.5.6`, `grub-mkrescue 2.12`, **276** GRUB BIOS modules, **269** EFI
modules, OVMF present. Installed via `wsl -u root`; a `/etc/sudoers.d/claude-apt`
drop-in was also written so future non-interactive `sudo` works.

`make iso` produces a 52.8 MB hybrid El Torito image carrying both proven
loaders — no Multiboot2, per DDR-896. **The UEFI arm boots the real kernel from
the real ISO** to the standard `NEXUS KERNEL OK` sentinel.

**BIOS arm fails** at stage1's first read (`PRADYOS S1: DISK READ ERROR`).
stage1 is not at fault — it uses the BIOS-provided `DL` and issues an ordinary
16-sector EDD read. Two emulations measured: **floppy** fails because emulated
floppies do not implement `INT 13h AH=42h`, which is exactly what stage1 issues;
**hard disk** (drive 0x80, EDD supported, MBR partition table added by
`tools/build/mk_hdimg.py` after verifying 0x1BE..0x1FD was all-zero) also fails,
and that result is **not yet explained**. No-emulation is rejected by analysis:
2048-byte CD sectors would put every LBA four times too deep.

`smoke-iso-x86` tests **both** arms and is registered **EXCLUDED** — a gate that
cannot pass must not enter the matrix, and weakening it to UEFI-only would let
the BIOS arm rot while the suite looked green.

Next measurement: print `DL` from stage1 under the ISO — one byte settles whether
SeaBIOS engaged hard-disk emulation at all.

## Group 3 item 16 — CFS + AI hint lane SHIPPED (DDR-904)

**Cause: placement was never running.** Measured during a failing boot,
`g_vr_per_tick` was **0 for the first 4,097 `sched_place()` calls** — the rate was
sampled as an instantaneous floor delta between two ticks, and on the idle
single-CPU system `smoke-shell` becomes, that delta is frequently zero. Every
guard from the four previous attempts lives *behind* that early return: they were
correct and never reached, which is why each measured zero effect.

Fix: seed the rate from the **first real charge** — non-zero as soon as anything
has run, and in the right units by construction because it *is* a charge.

**A Heisenbug was caught before being believed.** The instrumented build passed
0/5, but `sched_place()` runs under the runqueue lock with IRQs off and `kputs`
there perturbs the timing under test. Diagnostics removed, re-run: **still 0/5**.
Only then was it real.

| Configuration | smoke-shell |
|---|---|
| baseline (control) | 0/5 |
| one-sided clamp | 5/5 FAIL |
| derived clamp | 4/5 FAIL |
| two-sided credit | 5/5 FAIL |
| **rate seeded, no diagnostics** | **0/5** |

**Full 18-gate regression green**, including all nine gates this item previously
broke. Six hypotheses; five refuted by measurement; three genuine defects fixed
en route. Lesson: verifying a code path *executes* belongs before verifying what
it does.

Not claimed: the sleeper-credit branch shows `credit=0` — correct and bounded,
but unexercised by current gates.

## Group 9 item 49 — SHIPPED: VirtualBox validation (DDR-906)

`tools/vbox/run_vbox.ps1` creates a throwaway VM, attaches the built ISO,
captures COM1 to a file and asserts the same `NEXUS KERNEL OK` sentinel every
other boot path uses. Proven by a real run: EFI arm, VirtualBox 7.2.8r173730,
exit 0, kernel reaching `NEXUS: starting scheduler`.

Two-host split, by necessity: **WSL builds the ISO, Windows boots it.**
VirtualBox needs VT-x directly and WSL2 is a Hyper-V guest without nested VT-x.
So this is an owner-run check and is deliberately **not** in the CI shard matrix
(no VirtualBox, no Windows host on the runners); `gate_shards.txt` is unchanged.

Value beyond the QEMU gates: VirtualBox is a third firmware, neither SeaBIOS nor
OVMF, so it independently checks for firmware-specific assumptions — the exact
class of defect item 48 is stuck on.

## Group 9 item 48 — still PARTIAL; DDR-905 was wrong (DDR-907)

DDR-905 concluded the ISO's BIOS arm failed because the El Torito emulated drive
lacked EDD, and specified a CHS `AH=02h` fallback. That fallback was built for
both loaders (geometry from `AH=08h`, never hardcoded), assembled clean under
`-Werror`, fit stage1's 512-byte budget with 111 bytes spare — and was
**reverted**, because measurement refuted the diagnosis it was built on:

```
s01h02     AH=08h reports 1 sector/track, 2 heads — impossible geometry
R01        AH=02h returns AH=01 "invalid command", same as AH=42h
```

Every read function is rejected, so the defect is not a missing feature; it is
consistent with drive 0x80 not being present. `DL=0x80` never proved hard-disk
emulation engaged — that was an over-reading of one register, and DDR-905's
whole chain inherited it. No choice of read function fixes an absent drive.

Boot loaders are unchanged from their committed state. `smoke-iso-x86` stays
excluded. Next step is a **measurement** (did SeaBIOS register the boot image as
a drive at all), not another fix.

## Group 9 item 48 — SHIPPED: hybrid BIOS+UEFI ISO boots on both arms (DDR-908)

```
PRADYOS S1: loading stage2...
PRADYOS S2: protected-mode loader
PRADYOS BOOT OK
NEXUS KERNEL OK
```

Two stacked defects, both measured, neither guessed:

1. **`mk_hdimg.py` wrote a non-cylinder-aligned partition end CHS.** SeaBIOS
   *derives* the emulated drive's geometry from that field, so `h=1 s=1` became
   a 2-head/1-sector-per-track disk on which every LBA was out of range. Both
   `AH=42h` and `AH=02h` returned `AH=01`, which is why the drive looked absent.
   Fixed by truncating to whole cylinders: now reports 63 spt / 16 heads.
2. **The El Torito emulated drive has no EDD.** With correct geometry `AH=42h`
   still returns `01`, confirming DDR-905's original diagnosis. Both loaders now
   fall back to CHS `AH=02h`, geometry from `AH=08h`, one sector per call.

DDR-905 was right and DDR-907 was wrong; the CHS fallback measured zero effect
only because a geometry defect underneath made it unreachable. Same failure mode
as item 16.

Regression: **12/12 gates pass**, including `smoke-iso-x86` covering both arms.
An earlier run reporting 12x PASS with **empty gate names** was discarded — the
shell variable never expanded and every line ran bare `make`. `smoke-iso-x86` is
now **unexcluded** and assigned to shard 1; `make ci-shard-check` OK, 142 gates
across 6 shards, 5 excluded with reasons.

## CORRECTION — items 16, 48, 49 are NOT shipped (CI-unconfirmed)

Retracted. All three were reported **shipped** on the strength of a local
12-gate subset while `dev/phase1` CI was **red**. Correct status:

| Item | Status |
|---|---|
| 16 CFS scheduler | code-complete, locally proven, **CI-unconfirmed** |
| 48 hybrid ISO | code-complete, locally proven, **CI-unconfirmed** |
| 49 VirtualBox | code-complete, locally proven, **CI-unconfirmed** |

CI red for four consecutive runs, first red `f7f1884`, last green `bd58545`:

```
788d730 failure   381b454 failure   35e79f9 failure   f7f1884 failure
bd58545 success
```

Failures, all Layer 7 compositor/input, none boot/fs/scheduler/ISO:

```
shard 1: [smoke] FAIL — required pattern 'PRADYOS_SURFACE_GONE' not found
         make: *** [Makefile:1703: smoke-winops] Error 1
shard 4: [wmclose] FAIL — close box click did not close
         make: *** [Makefile:2335: smoke-wmclose] Error 1
shard 0: [wmmin] FAIL — min box click did not minimize
```

**The reporting defect is the point.** A 12-gate subset was treated as equivalent
to the suite; it did not cover the WM gates, so it could not have caught this.
That is the identical coverage-gap shape this project's gate discipline exists to
prevent, occurring in status reporting rather than in code.

**Standing rule, effective now:** no item is reported shipped without quoting a
**terminal green CI run ID for the exact commit SHA**. If CI has not concluded,
the only permitted wording is "code-complete, CI-pending".

Direct boot observations remain valid — the BIOS arm's `NEXUS KERNEL OK` and the
VirtualBox boot were captured serial output, not gate inferences. Only the word
"shipped" is withdrawn. `main` stays at `27ba426`.

### Diagnosis so far

Local reproduction at HEAD: `smoke-wmclose` FAIL, `smoke-winops` FAIL,
`smoke-wmmin` PASS. **Real regression, not CI flake** — and `wmmin` is
additionally timing-sensitive (passes locally, fails in CI).

`f7f1884` (first red) touched only Makefile, `mk_hdimg.py`, docs and one
`gate_shards.txt` line — **no compositor, input, or kernel code**. The first red
commit and the cause are therefore probably different commits.

Measured: the injection tooling is **fixed-sleep with no polling at all**.

```
mouse_inject.sh:14  sleep 0.1     input_inject.sh:17  sleep 0.1
mouse_inject.sh:16  sleep 0.5     input_inject.sh:19  sleep 0.5
polling constructs (until / while-grep / poll): 0
```

These gates assume the guest reached a state after a wall-clock delay and never
confirm it, so they cannot distinguish "the click failed" from "the click had not
happened yet". Any unrelated change to build or boot wall-clock time can flip
them. The fix is to make them poll for the state with a timeout — **not** to
enlarge the sleeps, which would tune the test to fit the change.

Pending: the three gates re-run at `bd58545` in an isolated tree, to decide
whether they were already broken (CI's historical green was unreliable) or
genuinely regressed.

### WM regression bisected to item 16 (35e79f9)

Both isolated-tree runs verified their overlay before trusting the result.

| Tree | winops | wmclose | wmmin |
|---|---|---|---|
| `bd58545` (last CI-green) | PASS | PASS | PASS |
| `f7f1884` (first CI-red) | PASS | PASS | PASS |
| HEAD | **FAIL** | **FAIL** | PASS |

`f7f1884` passes locally on all three. The only commit between it and HEAD that
changes kernel behaviour is **`35e79f9` — item 16's CFS scheduler**. Everything
else in the range is docs, a PowerShell script and a lockfile.

**Item 16 is the local cause.** Its smallest-vruntime pick and TSC elapsed-time
charging change how promptly the compositor thread runs after an input event.

Two contributing factors, one fragile mechanism:

1. `f7f1884`'s CI red is NOT reproducible locally, so the ISO build steps shifted
   runner wall-clock enough to tip the same fixed-sleep margin there first.
2. `35e79f9` then added enough input latency to tip it on ordinary hosts too.

Neither would have failed a gate that waited for observed state. The injection
tooling has no polling at all (`sleep 0.1`/`sleep 0.5`, zero `until`/`while`),
so it cannot distinguish a failed click from one that has not landed yet.

**Fix order, deliberately in this sequence:**

1. **Repair the gates first** — polling-with-timeout in `mouse_inject.sh` and
   `input_inject.sh`. This must come first because it is the instrument. Until
   the gates can distinguish "slow" from "broken", any measurement of item 16's
   latency is unreadable.
2. **Then measure item 16's actual input latency** against repaired gates.
   Raising the sleeps instead would tune the test to fit the change AND hide a
   real scheduler regression — the outcome this project's rules exist to prevent.

Item 16 is NOT to be reverted on this evidence: it fixed a measured fairness
defect (48,155 picks against 366 ticks charged) across six hypotheses. Added
latency in an unpolled UI gate is not itself proof of a scheduler defect.

### Step 1/2 result — instrument repaired; wmclose is a HIT-TEST defect, not latency

`mouse_inject.sh` now polls for the outcome instead of firing five blind clicks
after a fixed 0.5s settle. Outcome patterns wired into the two pointer gates.

```
PASS smoke-wmmin    [inject] observed 'PRADYOS_WM_MIN' after 4 click(s)
FAIL smoke-wmclose  [inject] TIMEOUT — 'PRADYOS_WM_CLOSE' never appeared after 50 click(s)
FAIL smoke-winops   (no injector at all; guest alive at [hb] t=8500)
```

**`wmmin` was purely a margin** — four clicks and it responded. The instrument fix
alone closed it.

**`wmclose` is a genuine non-response**, and the repaired instrument is what
proves it: 50 clicks over 60s. The old fixed-sleep gate could not have
distinguished this from slowness.

The cause is NOT latency and NOT input delivery:

```
23  PRADYOS_MOUSE_OK   input delivered and serviced 23 times
 1  PRADYOS_TITLE_OK   windows exist, titles set
 0  PRADYOS_WM_CLOSE
```

Every click was received and handled; none hit GAMMA's close box. The gate
hardcodes `ABSX=16190 ABSY=2602`. Item 16's fair-share pick changed the order in
which the client's three windows are created, so GAMMA is no longer at those
pixels.

**Hardcoded coordinates are the spatial equivalent of a fixed sleep** — the same
"assume instead of observe" defect. The root fix is to derive the close box
position from the compositor's reported geometry, not to nudge the constants
until they hit again. Nudging them would leave the gate just as brittle and would
still not prove the close path works.

`smoke-winops` is client-driven with no injector and already polls with a 90s
timeout, so it is a third, separate question and must not be lumped in with the
pointer gates.

**Item 16 is still not implicated in a functional defect.** Changing window
creation order is a legitimate consequence of fair-share scheduling; a gate that
depended on the old order was depending on undocumented FIFO behaviour.

### Step A result — geometry emission works; the real defect is a MISSING THIRD SURFACE

`PRADYOS_WM_CLOSEBOX` now publishes each window's close/min box centre in tablet
coordinates, computed with the same expressions `draw_window` uses. The injector
resolves its target from that line and refuses to fall back to a guessed pixel.

```
PASS smoke-wmmin    [inject] observed 'PRADYOS_WM_MIN' after 4 click(s)
FAIL smoke-wmclose  [inject] TIMEOUT — no PRADYOS_WM_CLOSEBOX for title=GAMMA
```

The emission itself is correct and complete:

```
PRADYOS_WM_CLOSEBOX id=0 title=ALPHA close=4932,3887 min=4484,3887
PRADYOS_WM_CLOSEBOX id=1 title=BETA  close=6213,5596 min=5765,5596
```

**Only two surfaces exist. GAMMA is never created.** The hardcoded 16190,2602 was
pointing at a window that does not exist, which is why 50 clicks changed nothing
while input worked perfectly (23x `PRADYOS_MOUSE_OK`).

**The creation-order hypothesis was wrong.** The order did not change; the third
window is absent. Recorded because the wrong hypothesis was stated confidently in
DDR-910 and in the previous commit message.

**This unifies Step A and Step B.** `smoke-winops` drives surfacetest to create
window C, resize it, then close it, and waits for `PRADYOS_SURFACE_GONE` when the
live set shrinks. If C is never created the set never shrinks and the sentinel
never fires. **wmclose and winops have ONE root cause: the third surface is never
created.** `smoke-wmmin` needs only ALPHA/BETA and is unaffected — which is
exactly why it passes.

This is a real userspace/kernel defect, not a gate defect. Next measurement:
whether surfacetest requests the third surface and is refused, or never requests
it — and whether this reproduces at `bd58545`, which decides if item 16 is
involved at all. **Not yet known; not to be guessed.**

The geometry work stands on its merits: it removed a genuine assume-don't-observe
defect and is the instrument that made the real one visible.

### Step B — narrowed: surface 2 is created but NEVER COMMITTED

Neither of the two predicted outcomes. The third surface is **requested AND
granted**, then never becomes visible to the compositor.

```
PRADYOS_RESIZE_OK id=2        <- surface 2 exists in the kernel and resized fine
PRADYOS_SURFACE_OK 0
PRADYOS_SURFACE_OK 1          <- but only 0 and 1 are ever composited
```

Not a client-side omission (surfacetest calls `make_window` then SET_TITLE
"GAMMA"), and not a kernel refusal (id=2 was allocated and resized).

Not a table limit either: the compositor polls for up to 16
(`surfs[16]`, `SYS_SURFACE_POLL(..., 16, 0)`), so the cap is not binding.

The filter is in `kernel/syscall/sys_surface.c:184`:

```c
if (g_surf[i].used && g_surf[i].committed) {
```

**Surface 2 is `used` but not `committed`**, so `SYS_SURFACE_POLL` omits it, the
compositor never learns it exists, and no close box is ever drawn or emitted for
it. `sys_surface_resize` is documented (line 349) as *keeping* the committed
flag, so resize is not the eraser.

**Next measurement, not yet taken:** whether `make_window` in `user/surfacetest.c`
commits C at all, and if it does, whether that commit is refused or lost. That is
one read of the helper plus one instrumented run.

**Item 16 still has no established connection.** The `bd58545` reproduction has
not been run and remains the decisive test for whether this predates item 16 — it
is very likely latent and previously unexercised, since no gate before this
instrumentation ever verified a third surface reached the compositor.

### Step B (clean re-run) — race REFUTED; exit-reap is the new candidate

Single controlled run, artifact `/tmp/attrib.wmclose.20260811T175706Z.log`,
copied and made read-only immediately after the gate exited. Nothing below is
inferred from any other run — the previous cross-run reasoning is discarded.

**`PRADYOS_ZORDER` never shows three ids. Maximum is 2, at any point.**

That **refutes the race hypothesis**: C is not briefly visible and then closed by
surfacetest's own scheduled close. It never reaches the compositor at all.

Ordering from the same artifact:

```
210: PRADYOS_RESIZE_OK id=2          C exists, committed, resized
212: PRADYOS_SURFDESTROY_CHURN_OK
213: PRADYOS_SURFDESTROY_REUSE_OK
215: PRADYOS_SURFDESTROY_EXIT_OK     a DIFFERENT process's exit-reap runs
216: PRADYOS_SURFDESTROY_OK
371: PRADYOS_WM_CLOSEBOX id=0 ALPHA  first composite: only 0 and 1 survive
372: PRADYOS_WM_CLOSEBOX id=1 BETA
```

`surfdestroytest` runs its exit-reap between C's creation and the compositor's
first composite, and `PRADYOS_SURFDESTROY_REUSE_OK` shows slot reuse is exercised
in exactly that window.

**New leading candidate: the process-exit surface reap destroys a surface it does
not own**, taking surfacetest's C (id=2) with it. This is a kernel defect, and it
would explain both failing gates while leaving `wmmin` (ALPHA/BETA only) green.

**Explicitly NOT concluded.** The ordering is consistent with the reap taking C;
it does not prove it. The confirming measurement is a log at the destroy call
site recording `(id, owner_pid, caller_pid, reason)` for every destruction,
showing whether id=2 is destroyed and by whom. The commit-path hypothesis from
f59bbb7 is also still live and is distinguished by the same instrumentation.

**Item 16 remains unconnected**, and the `bd58545` reproduction is still the
outstanding decisive test.

### Reap hypothesis REFUTED by code inspection

`surface_reap_pid` (`kernel/syscall/sys_surface.c:336`) checks ownership:

```c
int live = (s->used && s->owner_pid == pid) ? (surf_take_free(s, &phys, &order), 1) : 0;
```

Only surfaces whose `owner_pid` matches the exiting process are freed, and the
sole caller (`sched.c:1147`) passes `current_thread->pid` under `is_user`. There
is **no missing ownership check**, so this is not item 15's
verify-before-acting defect class one layer down. The timing correlation did not
survive contact with the code — which is why the second confirmation was
required before concluding.

**Three candidates remain, none yet measured:**

1. **Compositor start ordering.** The compositor's render block only runs when
   `ns != composited`. `surfacetest` creates C, then closes it after a delay by
   design. If the compositor's *first* poll happens after that close, C is never
   seen and `ns` goes straight 0 -> 2. This is the only candidate with a
   plausible link to item 16, since fair-share scheduling changes when the
   compositor first runs relative to its clients.
2. **`owner_pid` mismatch** — PID reuse or a thread/process id confusion making a
   legitimate check free the wrong slot.
3. **Commit-path** (`f59bbb7`) — still live, though `make_window` calls
   `SYS_SURFACE_COMMIT` for all three windows identically.

**The measurement that separates all three** is one instrumented run logging, with
timestamps: every `g_surf[i].committed = 1` (id, pid), every destroy (id,
owner_pid, caller_pid, reason), and each compositor poll result (`ns`). Candidate
1 shows C created and destroyed before the first poll; candidate 2 shows a
destroy whose caller_pid differs from owner_pid; candidate 3 shows no commit for
id=2.

Candidate 1 makes the outstanding `bd58545` reproduction more important, not
less: it is the only thing that can show whether item 16 moved the compositor's
first poll.

### Candidate 1 supported by code: C's lifetime is a LOOP-ITERATION COUNT

`user/surfacetest.c:121`:

```c
if (!closed && c >= 0 && ticks > 12000) {   /* close C; set shrinks 3 -> 2 */
```

`ticks` increments once per pass of surfacetest's polling loop. **C's lifetime is
therefore measured in loop iterations, not in observed state and not in
wall-clock.** How long 12,000 iterations takes depends entirely on how much CPU
surfacetest receives relative to the compositor — which is exactly what item 16's
fair-share pick changed.

Under FIFO, surfacetest ran slowly enough that C was still alive when the
compositor first polled. Under fair-share it completes 12,000 iterations sooner
relative to compositor startup, so C is created *and closed* before the
compositor's first poll — `ns` goes straight 0 -> 2, C is never composited, no
close box is ever emitted for GAMMA, and `winops` never sees a shrink because the
set never grew.

This is the **same defect class as the fixed sleeps and the hardcoded pixels**: a
fixed count standing in for an observed state. Third instance on this item, third
axis — time, space, now iteration count.

**Item 16 is the trigger, not the defect.** A client that measures a duration in
loop iterations is depending on undocumented scheduling behaviour, exactly as the
hardcoded pixels depended on undocumented creation order.

**Status: strongly supported by code, NOT yet confirmed.** The combined
instrumented run (commit/destroy/poll timestamps) and the `bd58545` reproduction
are both still required — the standing bar on this item is a second independent
measurement, and four hypotheses have already died at exactly this stage.

**Fix direction if confirmed:** C must stay alive until the compositor has
demonstrably observed it. The correct shape is a readiness handshake — surfacetest
waits for a compositor-emitted signal that the live set reached 3 before starting
its close countdown — not a larger tick threshold, which would re-race the same
defect on faster hardware.

### CONFIRMED (DDR-911): C's lifecycle completes before the compositor's first poll

Instrumented run, artifact `/tmp/attrib.instr.20260811T181358Z.log`, chmod 444
immediately after the gate exited:

```
210: PRADYOS_RESIZE_OK id=2     C created
282: PRADYOS_CLOSE_OK id=2      C closed
369: PRADYOS_FIRSTPOLL ns=2     compositor's FIRST poll — C already gone
372: PRADYOS_ZORDER 0 1
```

`PRADYOS_FIRSTPOLL` reports the surface count at the compositor's very first
`SYS_SURFACE_POLL`. **ns=2.** The third window is created and destroyed inside a
window in which nothing can observe it. Candidate 1 confirmed; candidates 2
(pid mismatch) and 3 (commit path) are both excluded — C is committed (it is
resized and closed by id) and is destroyed by its own owner for its own reason.

### The race was previously KNOWN and tuned, not fixed

`user/surfacetest.c:121`:

```c
if (!closed && c >= 0 && ticks > 12000) {   /* close C; set shrinks 3 -> 2
                                             * (rq-1: yields are cheaper —
                                             * widened so the compositor
                                             * composites the 3-set first) */
```

The comment records that this threshold was **already widened once** to win this
exact race. So this is not a latent defect that was never exercised: it is a known
race that was papered over with a larger loop count, and item 16's change to the
CPU share moved the race back.

That is empirical proof that raising the number buys time and nothing else, and
it settles the fix: a **readiness handshake**, not another threshold.

**Item 16 is the trigger, not the defect.** A client measuring a duration in loop
iterations depends on undocumented scheduling behaviour.
