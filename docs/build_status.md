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
**Deferred (ADR-023 §D8, binding trigger):** the context switch does not yet
save/restore FPU+XMM — correct only while one thread uses the FPU at a time (true
through 5d). Add per-thread `FXSAVE`/`FXRSTOR` before two ring-3 C/SSE processes
run concurrently (PRISM children in 5e). NSI append-only; ADR-021 W^X untouched.
Still deferred: NET-B (lwIP, §10). **Next: 5d pradyos-init (PID 1 + orphan
reaper).** Layer 6 (AETHER) begins after 5e + NET-B.
**Last updated:** 2026-06-27

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
| Hardware-info handoff struct | 🔴 NOT BUILT | 1 | E820 + CPUID gathered but not yet packaged for a kernel; blocked on Phase 2a |
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
| PRADYOS Extended Syscalls | 🔴 NOT BUILT | 6 | agent + sovereign (Phase 6) |
| ACPI table parser (RSDP/RSDT/XSDT) | 🟢 COMPLETE | 3 | `kernel/acpi/` (ADR-013): find RSDP, walk RSDT/XSDT, `acpi_find_table`. Unblocks MCFG/MADT/FADT. |
| PCIe Enumeration | 🟢 COMPLETE | 3 | `kernel/drivers/pcie/` (ADR-013): MCFG→ECAM, uncached map, bus-0 scan + device registry. q35: 7 devices incl. virtio-blk/net + VGA. |
| virtio transport (reusable) | 🟢 COMPLETE | 3 | `kernel/drivers/virtio/` (ADR-014): modern 1.0 — PCI caps, BAR/MMIO, status machine, feature negotiation, split virtqueues, notify, ISR. Shared by all virtio devices. |
| Block layer (generic) | 🟢 COMPLETE | 3 | `kernel/drivers/blk/blk.{c,h}`: device registry + read/write dispatch. |
| virtio-blk driver | 🟢 COMPLETE | 3 | `kernel/drivers/blk/virtio_blk.c` (ADR-014/015): interrupt-driven (INTx) read/write. **Multi-instance** (per-disk transport/queue/BAR window) + **per-device serialization** (busy + yield-wait). Verified: sector-0 MBR read, write/read round-trip, 2 disks concurrently. |
| NVMe Driver | 🔴 NOT BUILT | 3 | priority storage (registers with blk layer) |
| GPU Framebuffer | 🔴 NOT BUILT | 3 | linear framebuffer |
| Network Driver (virtio-net) | 🔴 NOT BUILT | 3 | device enumerated; reuses virtio transport |
| ACPI Power Management (FADT/MADT) | 🔴 NOT BUILT | 3 | parser ready; MADT→APIC, FADT→power |
| VFS Layer | 🟢 COMPLETE | 4 | `kernel/fs/vfs/` (ADR-015): driver registry + **mount table** (per-mount context vtable; FAT32/SFS/ext4 mountable side-by-side) + `open`/`create`/`read`/`write`/`unlink`/`readdir`, all capability-gated (CAP_FS_READ/WRITE via NCS) + per-thread write budget. Full mount-point namespace deferred. |
| FAT32 (read-write) | 🟢 COMPLETE | 4 | `kernel/fs/fat32/` (ADR-015): BPB parse, FAT chain, 8.3 + **VFAT long-name read** (ADR-020), nested paths. Read-write (4c): create/write/unlink, all-or-nothing alloc, read-back verify (`smoke-fs-rw`). **Timestamps** from RTC (4j). LFN *write* deferred (creates 8.3). |
| RTC / CMOS clock | 🟢 COMPLETE | 3 | `kernel/drivers/rtc/` (ADR-020): wall-clock via ports 0x70/0x71 (BCD/binary, 12/24h, stable read). `rtc_now` + `rtc_fat_datetime`; powers FS timestamps and later CLOCK_REALTIME. (Deferred Layer-3 item, pulled in at 4j.) |
| SOVEREIGN FS (SFS) | 🟢 COMPLETE | 4 | `kernel/fs/sfs/` (ADR-017/018): inode-based CoW B+tree, 4 KiB blocks. **4d:** format/mount/empty-root. **4e:** CoW B+tree create/lookup/open/readdir (split-on-insert; 10-file test). **4f:** file extents (write append/grow + read). **4g:** journal + atomic transactions (commit-record + mount replay). **4h:** snapshots — retained CoW roots; `sfs_open_version` reads a file as-of a snapshot. **4i:** inline LZ4 (`kernel/fs/sfs/lz4.c`, bounds-checked) — **per-extent** compression so compressed files still append; + ~4 KiB inode metadata tags (`sfs_set_tag`/`get_tag`). Verified: 128 KiB compressible → <32 blocks, byte-exact readback, tag survives remount. Next: ext4 read + FAT32 LFN (4j) → Layer 4 gate. Free-space B+tree / snapshot GC deferred. `CAP_FS_SFS_*` reserved. |
| SOVEREIGN FS (SFS) | 🔴 NOT BUILT | 4 | B+ tree, versioned |
| ext4 Compatibility | 🟢 COMPLETE | 4 | `kernel/fs/ext4/` (ADR-019, slice 4j): **read-only** (the Layer-4 scope; write is out of scope) — superblock, group descriptors, extent-mapped inodes (depth-0), linear dir scan, nested paths. Verified reading a host `mkfs.ext4 -d` volume (4th disk). Write, multi-level extents, block-mapped inodes deferred. |
| ELF64 loader + W^X (static) | 🟢 COMPLETE | 5a | `kernel/exec/elf.c` (ADR-021): validates ET_EXEC/x86-64, maps each PT_LOAD into a fresh per-process AS with p_flags→W^X perms (text RX, rodata R-NX, data RW-NX; **W+X rejected**), zero-fills BSS, 8 MiB RW-NX user stack + unmapped guard page, SysV `argc/argv/envp/auxv` frame; spawns a ring-3 thread (cap delivered in RDI). Bootstrapped via SFS: the embedded test ELF (`user/hello.asm`) is written to SFS then **loaded back from SFS** — prints `HELLO FROM RING-3`, exits via sys_exit. W^X negative regression (`user/wxviol.asm`: write to RX text → #PF err=0x7 → clean kill, kernel survives). Gate `smoke-user` PASS. COW fork / dynamic linking / AS-reaping deferred. |
| pradyos-init (PID 1) | 🔴 NOT BUILT | 5 | Rust |
| PRISM Shell | 🔴 NOT BUILT | 5 | POSIX + agent DSL |
| musl libc port | 🔴 NOT BUILT | 5 | + PRADYOS ext |
| prad package manager | 🔴 NOT BUILT | 5 | |
| AETHER Daemon | 🔴 NOT BUILT | 6 | core agent runtime |
| Ollama IPC Bridge | 🔴 NOT BUILT | 6 | feasibility OPEN — see ADR-004 |
| Cloud API Adapters | 🔴 NOT BUILT | 6 | Anthropic/OpenAI/Gemini |
| Agent Capability Enforcer | 🔴 NOT BUILT | 6 | kernel-backed |
| SOVEREIGN Gate Logic | 🔴 NOT BUILT | 6 | mode switching |
| Approval Queue System | 🔴 NOT BUILT | 6 | ring buffer + UI |
| Named Agents (KRYOS…SOLIN) | 🔴 NOT BUILT | 6f | 8 agents |
| Wayland Compositor | 🔴 NOT BUILT | 7 | wlroots-based Rust |
| SOVEREIGN MODE UI | 🔴 NOT BUILT | 7 | dark space theme |
| MANUAL MODE UI | 🔴 NOT BUILT | 7 | traditional desktop |
| Mode Toggle Animation | 🔴 NOT BUILT | 7 | 300ms cubic-bezier |
| Quantum Abstraction Layer | 🔴 NOT BUILT | 8 | future; citation unverified |
| AVX-512 memcpy (asm) | 🔴 NOT BUILT | 2 | with CPUID fallback |
| Syscall Entry (asm) | 🟢 COMPLETE | 2e | `arch/x86_64/syscall_entry.asm` — stack switch, save/restore, marshal, SYSRET. |
| IPC Zero-Copy (asm) | 🔴 NOT BUILT | 2d | VMOVDQU |

## ⏸ DEFERRED — tracked, not dropped (interim implementation in place or later phase)

These are intentionally postponed. The current build is functionally complete
without them; each has a concrete "build before" trigger so it is not forgotten.

| Deferred item | Interim / status | Build before |
|---------------|------------------|--------------|
| **APIC** (Local + I/O APIC, APIC timer) | legacy 8259 PIC + PIT active | SMP; MSI device interrupts. Unblocked by the ACPI/MADT parser (Phase 3). |
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
