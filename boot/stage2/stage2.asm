; boot/stage2/stage2.asm
; ============================================================================
; PRADYOS-BOOT — Stage 2 (real mode -> protected mode -> long mode -> kernel).
;
; Loaded by Stage 1 to physical 0x0000:0x7E00, entered in 16-bit real mode with
; CS=0 and DL = BIOS boot drive. Stage 2:
;   1. (real mode)  enable A20, walk the INT 15h E820 map, CPUID vendor + LM bit,
;                   and load the NEXUS kernel image from disk to physical 0x10000.
;   2. (real->32PM) load a GDT with 32-bit AND 64-bit descriptors, switch to
;                   32-bit protected mode, print "PRADYOS BOOT OK" (Phase 1 gate).
;   3. (32PM->64)   identity-map the low 1 GiB with 2 MiB pages, enable PAE +
;                   EFER.LME + paging, far-jump into 64-bit long mode.
;   4. (long mode)  jump to the kernel entry at 0x10000.
;
; Memory map this stage establishes (all in identity-mapped low RAM):
;   0x00010000  kernel image (flat binary, <= 32 KiB)        [loaded from LBA 17]
;   0x00070000  PML4   (4 KiB)
;   0x00071000  PDPT   (4 KiB)
;   0x00072000  PD     (4 KiB, 512 x 2 MiB = 1 GiB identity)
;   0x00200000  kernel stack top
;
; Honest scope notes (see ADR-005): early identity map only (no higher-half yet);
; kernel loaded flat at a low fixed address (real mode can't write >=1 MiB and we
; have no ELF parser yet); no kernel ELF parsing or hardware-info handoff struct
; yet — those are the next slices.
;
; References (verify, do not trust from memory):
;   - Intel SDM Vol.3 9.8.5 "Initializing IA-32e Mode" (PAE -> CR3 -> EFER.LME
;     -> CR0.PG -> far jump to a CS with the L bit set).
;   - AMD64 APM Vol.2 5.3 (4-level paging, 2 MiB PDE with PS bit).
;   - OSDev "Setting Up Long Mode".
;
; Build: nasm -f bin stage2.asm -o stage2.bin   (must stay <= 8 KiB)
; ============================================================================

BITS 16
ORG 0x7E00

COM1        equ 0x3F8
CODE32_SEL  equ 0x08            ; GDT entry 1
DATA32_SEL  equ 0x10            ; GDT entry 2
CODE64_SEL  equ 0x18            ; GDT entry 3 (L=1)
DATA64_SEL  equ 0x20            ; GDT entry 4

KERNEL_PHYS equ 0x00010000
KSTACK_TOP  equ 0x00200000
PML4        equ 0x00070000
PDPT        equ 0x00071000
PD          equ 0x00072000

; Boot -> kernel handoff struct (see kernel/boot_info.h). Lives at a fixed low
; address (usable conventional RAM 0x500..0x7C00) so the kernel can find it.
; Layout: magic(4) count(4) cpu_vendor(16) long_mode(4) reserved(4) then the
; E820 entries (24 bytes each) at offset 32.
BOOT_INFO   equ 0x00004000
BOOT_MAGIC  equ 0x59445250     ; 'PRDY'

stage2_start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl        ; Stage 1 left the boot drive in DL

    call serial_init
    call init_boot_info

    mov si, msg_s2
    call puts16

    call enable_a20
    mov si, msg_a20
    call puts16

    call do_e820
    call do_cpuid
    call load_kernel

    mov si, msg_pm
    call puts16

; --- Enter 32-bit protected mode. (Intel SDM Vol.3 9.9.1.) -------------------
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1                   ; CR0.PE
    mov cr0, eax
    jmp CODE32_SEL:pm_entry

; ============================  16-bit helpers  ==============================

; Zero the 32-byte boot_info header and stamp the magic. ES is already 0.
init_boot_info:
    cld
    mov di, BOOT_INFO
    xor ax, ax
    mov cx, 16                  ; 32 bytes
    rep stosw
    mov dword [BOOT_INFO], BOOT_MAGIC
    ret

; Fast A20 via System Control Port A (0x92); keep bit 0 clear (fast reset).
enable_a20:
    in al, 0x92
    test al, 0x02
    jnz .done
    or al, 0x02
    and al, 0xFE
    out 0x92, al
.done:
    ret

; Load the kernel image from disk (INT 13h/AH=42h) to physical 0x10000.
load_kernel:
    mov si, msg_ldk
    call puts16
    mov si, kernel_dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .err
    ret
.err:
    mov si, msg_ldk_err
    call puts16
.halt:
    hlt
    jmp .halt

; INT 15h EAX=E820h walk -> e820_buf / e820_count, then print the count.
do_e820:
    mov di, BOOT_INFO + 32      ; entries land in the handoff struct
    xor ebx, ebx
    mov word [e820_count], 0
.loop:
    mov eax, 0x0000E820
    mov edx, 0x534D4150
    mov ecx, 24
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done
    inc word [e820_count]
    add di, 24
    test ebx, ebx
    jz .done
    cmp word [e820_count], 32
    jae .done
    jmp .loop
.done:
    movzx eax, word [e820_count]
    mov [BOOT_INFO + 4], eax    ; store the count into the handoff struct
    mov si, msg_e820
    call puts16
    mov al, [e820_count]
    call print_hex8
    mov si, crlf
    call puts16
    ret

; CPUID vendor string (leaf 0) + long-mode support (leaf 80000001h EDX.29).
do_cpuid:
    mov si, msg_cpu
    call puts16
    xor eax, eax
    cpuid
    mov [BOOT_INFO + 8], ebx    ; vendor order is EBX, EDX, ECX
    mov [BOOT_INFO + 12], edx
    mov [BOOT_INFO + 16], ecx
    mov byte [BOOT_INFO + 20], 0
    mov si, BOOT_INFO + 8
    call puts16
    mov si, crlf
    call puts16

    mov si, msg_lmq
    call puts16
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .nolm
    mov dword [BOOT_INFO + 24], 1
    mov si, msg_yes
    call puts16
    ret
.nolm:
    mov dword [BOOT_INFO + 24], 0
    mov si, msg_no
    call puts16
    ret

serial_init:
    mov dx, COM1 + 1
    mov al, 0x00
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x80
    out dx, al
    mov dx, COM1 + 0
    mov al, 0x01
    out dx, al
    mov dx, COM1 + 1
    mov al, 0x00
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x03
    out dx, al
    mov dx, COM1 + 2
    mov al, 0xC7
    out dx, al
    mov dx, COM1 + 4
    mov al, 0x0B
    out dx, al
    ret

serial_putc:                    ; AL = byte; preserves AX, DX
    push dx
    push ax
    mov ah, al
.w:
    mov dx, COM1 + 5
    in al, dx
    test al, 0x20
    jz .w
    mov dx, COM1 + 0
    mov al, ah
    out dx, al
    pop ax
    pop dx
    ret

bios_putc:                      ; AL = byte; preserves AX, BX
    push bx
    push ax
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop ax
    pop bx
    ret

puts16:                         ; DS:SI -> NUL string
    push ax
    push si
.l:
    lodsb
    test al, al
    jz .e
    call serial_putc
    call bios_putc
    jmp .l
.e:
    pop si
    pop ax
    ret

print_hex8:                     ; AL -> two hex digits
    push ax
    push bx
    mov bl, al
    shr al, 4
    call .nyb
    mov al, bl
    and al, 0x0F
    call .nyb
    pop bx
    pop ax
    ret
.nyb:
    and al, 0x0F
    cmp al, 10
    jb .dig
    add al, 'A' - 10
    jmp .emit
.dig:
    add al, '0'
.emit:
    call serial_putc
    call bios_putc
    ret

; ============================  32-bit code  =================================
BITS 32
pm_entry:
    mov ax, DATA32_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x90000

    ; Phase 1 gate: VGA + serial sentinel.
    mov esi, msg_ok
    mov edi, 0xB8000
    mov ah, 0x0F
.vga:
    lodsb
    test al, al
    jz .vga_done
    mov [edi], al
    mov [edi + 1], ah
    add edi, 2
    jmp .vga
.vga_done:
    mov esi, msg_ok
    call puts32
    mov esi, crlf
    call puts32

    mov esi, msg_lm
    call puts32

    call build_page_tables
    call enter_long_mode
    jmp CODE64_SEL:long_entry

serial_putc32:                  ; AL = byte; preserves EAX, EDX
    push edx
    push eax
    mov ah, al
.w:
    mov dx, COM1 + 5
    in al, dx
    test al, 0x20
    jz .w
    mov dx, COM1 + 0
    mov al, ah
    out dx, al
    pop eax
    pop edx
    ret

puts32:                         ; ESI -> NUL string
    push eax
    push esi
.l:
    lodsb
    test al, al
    jz .e
    call serial_putc32
    jmp .l
.e:
    pop esi
    pop eax
    ret

; Identity-map the low 1 GiB: PML4[0]->PDPT[0]->PD, PD has 512 x 2 MiB pages.
build_page_tables:
    cld
    mov edi, PML4
    xor eax, eax
    mov ecx, 0x3000 / 4         ; zero PML4 + PDPT + PD
    rep stosd

    mov dword [PML4], PDPT | 0x3 ; present + rw
    mov dword [PDPT], PD | 0x3

    mov edi, PD
    mov eax, 0x83               ; phys 0 | present | rw | PS (2 MiB page)
    mov ecx, 512
.fill:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000           ; next 2 MiB
    add edi, 8
    loop .fill
    ret

; PAE -> CR3 -> EFER.LME -> CR0.PG. (Intel SDM Vol.3 9.8.5.)
enter_long_mode:
    mov eax, cr4
    or eax, 1 << 5             ; CR4.PAE
    mov cr4, eax

    mov eax, PML4
    mov cr3, eax

    mov ecx, 0xC0000080        ; IA32_EFER
    rdmsr
    or eax, 1 << 8             ; EFER.LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31            ; CR0.PG
    mov cr0, eax
    ret

; ============================  64-bit code  =================================
BITS 64
long_entry:
    mov ax, DATA64_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, KSTACK_TOP
    mov edi, BOOT_INFO          ; SysV arg 1 -> kmain(struct boot_info *)
    mov rax, KERNEL_PHYS
    jmp rax

; ============================  data + GDT  ==================================
msg_s2   db 13, 10, "PRADYOS S2: protected-mode loader", 13, 10, 0
msg_a20  db "  A20: enabled (fast, port 0x92)", 13, 10, 0
msg_e820 db "  E820 map entries (hex): 0x", 0
msg_cpu  db "  CPU vendor: ", 0
msg_lmq  db "  Long mode supported: ", 0
msg_yes  db "yes", 13, 10, 0
msg_no   db "no", 13, 10, 0
msg_ldk  db "  loading kernel @0x10000...", 13, 10, 0
msg_ldk_err db "  KERNEL LOAD ERROR", 13, 10, 0
msg_pm   db "  switching to protected mode...", 13, 10, 0
msg_ok   db "PRADYOS BOOT OK", 0
msg_lm   db "PRADYOS S2: enabling long mode, jumping to NEXUS...", 13, 10, 0
crlf     db 13, 10, 0

boot_drive db 0

; Disk Address Packet: read 64 sectors (32 KiB) from LBA 17 -> 0x1000:0x0000.
kernel_dap:
    db 0x10
    db 0x00
    dw 64
    dw 0x0000                  ; offset
    dw 0x1000                  ; segment (-> physical 0x10000)
    dq 17                      ; starting LBA (matches the Makefile image layout)

; GDT: null, 32-bit code/data, 64-bit code (L=1) / data.
gdt_start:
    dq 0x0000000000000000
gdt_code32:
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
gdt_data32:
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_code64:
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00   ; flags 0xAF: G=1, L=1
gdt_data64:
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; --- scratch (E820 entries now go straight into the boot_info struct) --------
e820_count: dw 0
