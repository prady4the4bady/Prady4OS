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
