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
