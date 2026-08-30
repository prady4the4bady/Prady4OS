; arch/x86_64/user_image.asm — embed the built ring-3 ELF into the kernel image.
; ============================================================================
; The Phase 5a gate loads the user program *from SFS*: the kernel writes these
; embedded bytes to a freshly formatted SFS volume, then reads them back and
; runs them through the ELF loader. This file is the bootstrap source of those
; bytes (there is no host mkfs.sfs yet). incbin pulls in build/hello.elf, which
; the Makefile links before assembling this unit.
; ============================================================================

BITS 64

section .rodata
global hello_elf
global hello_elf_end
hello_elf:
    incbin "build/hello.elf"
hello_elf_end:

; W^X negative-regression program (writes to its own text -> #PF -> clean kill).
global wx_elf
global wx_elf_end
wx_elf:
    incbin "build/wxviol.elf"
wx_elf_end:

; Phase 5b syscall test program (grows per slice; exercises read/write/open/...).
global systest_elf
global systest_elf_end
systest_elf:
    incbin "build/systest.elf"
systest_elf_end:

; Phase 5b slice 7 execve target: the kernel writes this to the FAT32 root as
; /EXECTEST.ELF; systest then SYS_EXECVE's it (replaces its own image).
global exectest_elf
global exectest_elf_end
exectest_elf:
    incbin "build/exectest.elf"
exectest_elf_end:

; PROC-D step 1: SYS_SET_TLS + SYS_WRITEV probe. Sets FS base to a stack scratch
; slot, round-trips a value through %fs:0, then gathers two iovecs to fd 1.
global tlstest_elf
global tlstest_elf_end
tlstest_elf:
    incbin "build/tlstest.elf"
tlstest_elf_end:

; PROC-D step 3: the first ring-3 C program, statically linked against the musl
; subset. Written to SFS and loaded back; prints "PRADYOS_MUSL_OK ..." via printf.
global cmusl_elf
global cmusl_elf_end
cmusl_elf:
    incbin "build/cmusl.elf"
cmusl_elf_end:

; 5d: FPU-context-switch regression. Two instances run concurrently; each checks
; its XMM0 survives a yield to the other. Proves per-thread FXSAVE (ADR-023 §D8).
global fputest_elf
global fputest_elf_end
fputest_elf:
    incbin "build/fputest.elf"
fputest_elf_end:

; 5d: pradyos-init (PID 1), a musl C program. Reaps children; forks one that
; exits 42. Prints "PRADYOS_INIT_OK ..." then "init: reaped PID=N exit=42".
global init_elf
global init_elf_end
init_elf:
    incbin "build/init.elf"
init_elf_end:

; 5e: PRISM shell (musl C). The kernel writes it to SFS and loads it as init's
; child (execve-from-FAT32 of a large image is deferred — see ADR-024 §D5).
global prism_elf
global prism_elf_end
prism_elf:
    incbin "build/prism.elf"
prism_elf_end:

; L6: AETHER daemon (PID-2, CAP_SOVEREIGN). Kernel writes it to SFS + loads it as
; init's child, like PRISM; it owns the global mode + approve authority.
global aether_daemon_elf
global aether_daemon_elf_end
aether_daemon_elf:
    incbin "build/aether_daemon.elf"
aether_daemon_elf_end:

; L6: AETHER agent template (CAP_AGENT). Loaded on demand by SYS_SPAWN_AGENT from
; these embedded bytes (elf_load) — the SFS mount is gone by scheduler time.
global agent_base_elf
global agent_base_elf_end
agent_base_elf:
    incbin "build/agent_base.elf"
agent_base_elf_end:

; L7 (DDR-703): ring-3 keyboard input reader. Polls SYS_INPUT_POLL.
global inputtest_elf
global inputtest_elf_end
inputtest_elf:
    incbin "build/inputtest.elf"
inputtest_elf_end:

; L7 (DDR-704): in-house sovereign-desktop compositor. Renders over the FB +
; reacts to the keyboard; spawned with CAP_SOVEREIGN so it can flip the mode.
global compositor_elf
global compositor_elf_end
compositor_elf:
    incbin "build/compositor.elf"
compositor_elf_end:

; L7 (DDR-706): per-client surface test window. Creates+commits a surface.
global surfacetest_elf
global surfacetest_elf_end
surfacetest_elf:
    incbin "build/surfacetest.elf"
surfacetest_elf_end:

; L7 (DDR-729): surface lifecycle/destroy test — churn, reuse, exit-reclamation.
global surfdestroytest_elf
global surfdestroytest_elf_end
surfdestroytest_elf:
    incbin "build/surfdestroytest.elf"
surfdestroytest_elf_end:

; L7 (DDR-730): per-agent live-metrics probe.
global agentmetricstest_elf
global agentmetricstest_elf_end
agentmetricstest_elf:
    incbin "build/agentmetricstest.elf"
agentmetricstest_elf_end:

; L6/7 (DDR-731): CAP_NET socket-authority probe.
global capnettest_elf
global capnettest_elf_end
capnettest_elf:
    incbin "build/capnettest.elf"
capnettest_elf_end:

; fs (DDR-739): per-process root-mount probe.
global rootmounttest_elf
global rootmounttest_elf_end
rootmounttest_elf:
    incbin "build/rootmounttest.elf"
rootmounttest_elf_end:

; fs (DDR-744): ring-3 file-lifecycle probe (O_CREAT open + SYS_UNLINK on SFS).
global fsrmtest_elf
global fsrmtest_elf_end
fsrmtest_elf:
    incbin "build/fsrmtest.elf"
fsrmtest_elf_end:

; DDR-973: FAT32 multi-cluster read regression probe (rooted at the FAT mount).
global fat32mctest_elf
global fat32mctest_elf_end
fat32mctest_elf:
    incbin "build/fat32mctest.elf"
fat32mctest_elf_end:

; DDR-990: two-CPU connect/close hammer — the positive proof of the DDR-987 fix
; that the gate suite cannot supply at a ~1/20 base rate.
global nethammer_elf
global nethammer_elf_end
nethammer_elf:
    incbin "build/nethammer.elf"
nethammer_elf_end:

; DDR-991: PS/2 modifier / extended-key probe (Group E input foundation).
global modkeystest_elf
global modkeystest_elf_end
modkeystest_elf:
    incbin "build/modkeystest.elf"
modkeystest_elf_end:

; DDR-866: ring-3 ftruncate probe (Group 3 item 20)
global stackdemand_elf
global stackdemand_elf_end
stackdemand_elf:
    incbin "build/stackdemand.elf"
stackdemand_elf_end:

global ftrunctest_elf
global ftrunctest_elf_end
ftrunctest_elf:
    incbin "build/ftrunctest.elf"
ftrunctest_elf_end:

; DDR-962: SYS_RENAME probe on the SFS root (gate smoke-rename-sfs). Fits only
; because DDR-960 raised the stage-2 read window; see that DDR before adding
; another of these.
global renametest_elf
global renametest_elf_end
renametest_elf:
    incbin "build/renametest.elf"
renametest_elf_end:

; DDR-870: perf benchmark probe (Group 8 items 44/45)
global benchtest_elf
global benchtest_elf_end
benchtest_elf:
    incbin "build/benchtest.elf"
benchtest_elf_end:

; DDR-801 (R3): per-destination egress audit probe.
global egressaudittest_elf
global egressaudittest_elf_end
egressaudittest_elf:
    incbin "build/egressaudittest.elf"
egressaudittest_elf_end:

; DDR-800 (R1): sovereign-egress audit probe.
global hkdftest_elf
global hkdftest_elf_end
hkdftest_elf:
    incbin "build/hkdftest.elf"
hkdftest_elf_end:

global x25519test_elf
global x25519test_elf_end
x25519test_elf:
    incbin "build/x25519test.elf"
x25519test_elf_end:

global sha512test_elf
global sha512test_elf_end
sha512test_elf:
    incbin "build/sha512test.elf"
sha512test_elf_end:

global aeadtest_elf
global aeadtest_elf_end
aeadtest_elf:
    incbin "build/aeadtest.elf"
aeadtest_elf_end:

global ed25519test_elf
global ed25519test_elf_end
ed25519test_elf:
    incbin "build/ed25519test.elf"
ed25519test_elf_end:

global acctest_elf
global acctest_elf_end
acctest_elf:
    incbin "build/acctest.elf"
acctest_elf_end:


global agstest_elf
global agstest_elf_end
agstest_elf:
    incbin "build/agstest.elf"
agstest_elf_end:

global accrottest_elf
global accrottest_elf_end
accrottest_elf:
    incbin "build/accrottest.elf"
accrottest_elf_end:

global vaulttest_elf
global vaulttest_elf_end
vaulttest_elf:
    incbin "build/vaulttest.elf"
vaulttest_elf_end:

global agentmemtest_elf
global agentmemtest_elf_end
agentmemtest_elf:
    incbin "build/agentmemtest.elf"
agentmemtest_elf_end:

global ckpttest_elf
global ckpttest_elf_end
ckpttest_elf:
    incbin "build/ckpttest.elf"
ckpttest_elf_end:

global spawndepthtest_elf
global spawndepthtest_elf_end
spawndepthtest_elf:
    incbin "build/spawndepthtest.elf"
spawndepthtest_elf_end:

global actiondagtest_elf
global actiondagtest_elf_end
actiondagtest_elf:
    incbin "build/actiondagtest.elf"
actiondagtest_elf_end:

; DDR-1015: Section 3C ACTION_READ_FILE, end to end (submit -> verdict -> read).
global actionreadtest_elf
global actionreadtest_elf_end
actionreadtest_elf:
    incbin "build/actionreadtest.elf"
actionreadtest_elf_end:

; DDR-1016: Section 3C ACTION_DELETE_FILE -- the first FORCE-PENDING type, so its
; gate asserts the opposite of the one above: the verdict stays PENDING and the
; file survives.
global actiondeltest_elf
global actiondeltest_elf_end
actiondeltest_elf:
    incbin "build/actiondeltest.elf"
actiondeltest_elf_end:

; DDR-1017: Section 3C ACTION_SPAWN_PROCESS -- force-pending like DELETE_FILE, but
; the effect is asked of the kernel via wait4(WNOHANG) rather than the filesystem.
global actionspawntest_elf
global actionspawntest_elf_end
actionspawntest_elf:
    incbin "build/actionspawntest.elf"
actionspawntest_elf_end:

; DDR-1018: Section 3C ACTION_QUERY_MEMORY -- auto-approving (DDR-1015 shape),
; executed through the CAP_MEMORY agent-memory NSI 82/83.
global actionquerytest_elf
global actionquerytest_elf_end
actionquerytest_elf:
    incbin "build/actionquerytest.elf"
actionquerytest_elf_end:

global coderewritetest_elf
global coderewritetest_elf_end
coderewritetest_elf:
    incbin "build/coderewritetest.elf"
coderewritetest_elf_end:

global auditchaintest_elf
global auditchaintest_elf_end
auditchaintest_elf:
    incbin "build/auditchaintest.elf"
auditchaintest_elf_end:

global invarianttest_elf
global invarianttest_elf_end
invarianttest_elf:
    incbin "build/invarianttest.elf"
invarianttest_elf_end:
global lockboxtest_elf
global lockboxtest_elf_end
lockboxtest_elf:
    incbin "build/lockboxtest.elf"
lockboxtest_elf_end:

global sha256test_elf
global sha256test_elf_end
sha256test_elf:
    incbin "build/sha256test.elf"
sha256test_elf_end:

global sigpipetest_elf
global sigpipetest_elf_end
sigpipetest_elf:
    incbin "build/sigpipetest.elf"
sigpipetest_elf_end:

global privacynettest_elf
global privacynettest_elf_end
privacynettest_elf:
    incbin "build/privacynettest.elf"
privacynettest_elf_end:

global sovegresstest_elf
global sovegresstest_elf_end
sovegresstest_elf:
    incbin "build/sovegresstest.elf"
sovegresstest_elf_end:

; DDR-796 (BUG-1): SYS_CLOCK monotonicity probe under SMP.
global rtcmonotest_elf
global rtcmonotest_elf_end
rtcmonotest_elf:
    incbin "build/rtcmonotest.elf"
rtcmonotest_elf_end:

; F#68/DDR-795: sealed metric-region probe (reads the root, then proves a
; ring-3 write to it faults).
global metrictest_elf
global metrictest_elf_end
metrictest_elf:
    incbin "build/metrictest.elf"
metrictest_elf_end:

; sys (DDR-748): SYS_SYSINFO CPU/system introspection probe.
global sysinfotest_elf
global sysinfotest_elf_end
sysinfotest_elf:
    incbin "build/sysinfotest.elf"
sysinfotest_elf_end:

; sys (DDR-749): SYS_TIME wall-clock probe.
global timetest_elf
global timetest_elf_end
timetest_elf:
    incbin "build/timetest.elf"
timetest_elf_end:

; sys (DDR-750): SYS_DMESG kernel-log read-back probe.
global dmesgtest_elf
global dmesgtest_elf_end
dmesgtest_elf:
    incbin "build/dmesgtest.elf"
dmesgtest_elf_end:

; proc (DDR-755): SYS_KILL fork/kill/reap probe.
global killtest_elf
global killtest_elf_end
killtest_elf:
    incbin "build/killtest.elf"
killtest_elf_end:

; proc (DDR-756): SYS_SETNAME self-rename probe.
global setnametest_elf
global setnametest_elf_end
setnametest_elf:
    incbin "build/setnametest.elf"
setnametest_elf_end:

; sec (DDR-758): hostile-syscall fuzz probe.
global syscallfuzz_elf
global syscallfuzz_elf_end
syscallfuzz_elf:
    incbin "build/syscallfuzz.elf"
syscallfuzz_elf_end:

; fs (DDR-760): persistent SFS-root probe.
global sfsroottest_elf
global sfsroottest_elf_end
sfsroottest_elf:
    incbin "build/sfsroottest.elf"
sfsroottest_elf_end:

; fs (DDR-764): ring-3 large-write probe.
global bigwritetest_elf
global bigwritetest_elf_end
bigwritetest_elf:
    incbin "build/bigwritetest.elf"
bigwritetest_elf_end:
