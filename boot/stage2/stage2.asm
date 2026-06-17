; boot/stage2/stage2.asm
; ============================================================================
; PRADYOS-BOOT — Stage 2 (real mode -> 32-bit protected mode), Phase 1.
;
; Loaded by Stage 1 to physical 0x0000:0x7E00 and entered in 16-bit real mode
; with CS=0. Stage 2:
;   1. enables the A20 line (fast A20 via port 0x92),
;   2. queries the BIOS memory map (INT 15h, EAX=E820h),
;   3. detects the CPU vendor + long-mode capability via CPUID,
;   4. loads a flat GDT and switches into 32-bit protected mode,
;   5. prints "PRADYOS BOOT OK" from protected mode (serial COM1 + VGA text),
;      then halts.
;
; Long mode itself (PML4 paging, the 64-bit GDT, the kernel ELF load and
; handoff struct) is Phase 2a, not here. This stage proves the real-mode ->
; protected-mode pipeline and gathers the hardware facts the kernel will need.
;
; All progress is echoed to COM1 (serial) for the headless smoke test and to
; the BIOS console (INT 10h) while still in real mode. Once in protected mode
; the BIOS is gone, so output there is direct port I/O + the VGA text buffer.
;
; References (verify, do not trust from memory):
;   - OSDev "A20 Line" (fast A20 via System Control Port A, 0x92).
;   - OSDev "Detecting Memory (x86)" — INT 15h EAX=E820h, EDX='SMAP', ECX=24,
;     ES:DI buffer, EBX continuation.
;   - Intel SDM Vol.2 CPUID: leaf 0 (vendor in EBX,EDX,ECX), leaf 80000001h
;     (EDX bit 29 = Long Mode / IA-32e).
;   - Intel SDM Vol.3 9.9.1 "Switching to Protected Mode" (load GDT, set
;     CR0.PE, far jump to reload CS).
;
; UNCERTAIN / honest scope notes:
;   - CPUID presence is assumed (true for every x86_64-capable CPU PRADYOS
;     targets); the EFLAGS.ID toggle check is omitted deliberately.
;   - Full SMT/core topology needs CPUID leaf 0Bh/1Fh; here we only report the
;     vendor string and the long-mode bit. Topology is a Phase 2c concern.
;   - A20 is enabled but not exhaustively verified (QEMU enables it anyway).
;
; Build: nasm -f bin stage2.asm -o stage2.bin   (must stay <= 8 KiB)
; ============================================================================

BITS 16
ORG 0x7E00

COM1      equ 0x3F8
CODE_SEL  equ 0x08             ; GDT entry 1 (32-bit code)
DATA_SEL  equ 0x10             ; GDT entry 2 (32-bit data)

stage2_start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00             ; reuse the low stack; Stage 2 lives above 0x7E00

    call serial_init           ; harmless re-init (Stage 1 already did this)

    mov si, msg_s2
    call puts16

    call enable_a20
    mov si, msg_a20
    call puts16

    call do_e820

    call do_cpuid

    mov si, msg_pm
    call puts16

; --- Switch to 32-bit protected mode. (Intel SDM Vol.3 9.9.1.) ---------------
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1                  ; CR0.PE
    mov cr0, eax
    jmp CODE_SEL:pm_entry      ; far jump reloads CS with the 32-bit selector

; ============================  16-bit helpers  ==============================

; A20 via fast method: set bit 1 of System Control Port A (0x92), being careful
; NOT to set bit 0 (which would trigger a fast CPU reset).
enable_a20:
    in al, 0x92
    test al, 0x02
    jnz .done
    or al, 0x02
    and al, 0xFE
    out 0x92, al
.done:
    ret

; INT 15h EAX=E820h walk. Stores up to 32 raw 24-byte entries into e820_buf and
; records the count in e820_count, then prints the count.
do_e820:
    mov di, e820_buf
    xor ebx, ebx
    mov word [e820_count], 0
.loop:
    mov eax, 0x0000E820
    mov edx, 0x534D4150        ; 'SMAP'
    mov ecx, 24
    int 0x15
    jc .done                   ; CF set => unsupported (first call) or finished
    cmp eax, 0x534D4150
    jne .done
    inc word [e820_count]
    add di, 24
    test ebx, ebx              ; EBX=0 => this was the last entry
    jz .done
    cmp word [e820_count], 32  ; don't overflow the buffer
    jae .done
    jmp .loop
.done:
    mov si, msg_e820
    call puts16
    mov al, [e820_count]
    call print_hex8
    mov si, crlf
    call puts16
    ret

; CPUID: vendor string (leaf 0) + long-mode support (leaf 80000001h, EDX.29).
do_cpuid:
    mov si, msg_cpu
    call puts16
    xor eax, eax
    cpuid
    mov [vendor_buf + 0], ebx  ; vendor order is EBX, EDX, ECX
    mov [vendor_buf + 4], edx
    mov [vendor_buf + 8], ecx
    mov byte [vendor_buf + 12], 0
    mov si, vendor_buf
    call puts16
    mov si, crlf
    call puts16

    mov si, msg_lm
    call puts16
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29          ; Long Mode bit
    jz .nolm
    mov si, msg_yes
    call puts16
    ret
.nolm:
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

serial_putc:                   ; AL = byte; preserves AX, DX
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

bios_putc:                     ; AL = byte; preserves AX, BX
    push bx
    push ax
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop ax
    pop bx
    ret

puts16:                        ; DS:SI -> NUL-terminated string
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

print_hex8:                    ; AL = byte -> two hex digits
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
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x90000           ; conventional RAM well above our image, below EBDA

    ; VGA text buffer: write the sentinel at the top-left, white-on-black.
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

    ; Serial: the headless smoke test greps COM1 for this.
    mov esi, msg_ok
.ser:
    lodsb
    test al, al
    jz .ser_done
    call serial_putc32
    jmp .ser
.ser_done:
    mov al, 13
    call serial_putc32
    mov al, 10
    call serial_putc32

    cli
.hang:
    hlt
    jmp .hang

serial_putc32:                 ; AL = byte; preserves EAX, EDX
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

; ============================  data + GDT  ==================================
msg_s2  db 13, 10, "PRADYOS S2: protected-mode loader", 13, 10, 0
msg_a20 db "  A20: enabled (fast, port 0x92)", 13, 10, 0
msg_e820 db "  E820 map entries (hex): 0x", 0
msg_cpu db "  CPU vendor: ", 0
msg_lm  db "  Long mode supported: ", 0
msg_yes db "yes", 13, 10, 0
msg_no  db "no", 13, 10, 0
msg_pm  db "  switching to protected mode...", 13, 10, 0
msg_ok  db "PRADYOS BOOT OK", 0
crlf    db 13, 10, 0

; Flat GDT: null, 32-bit ring-0 code (base 0, limit 4 GiB), 32-bit ring-0 data.
gdt_start:
    dq 0x0000000000000000      ; null descriptor
gdt_code:
    dw 0xFFFF                  ; limit 15:0
    dw 0x0000                  ; base 15:0
    db 0x00                    ; base 23:16
    db 0x9A                    ; access: present, ring0, code, exec/read
    db 0xCF                    ; flags: G=1,D=1 + limit 19:16 = F
    db 0x00                    ; base 31:24
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92                    ; access: present, ring0, data, read/write
    db 0xCF
    db 0x00
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1 ; limit
    dd gdt_start               ; base (linear; ORG 0x7E00, segment base 0)

; --- scratch buffers ---------------------------------------------------------
; A flat `-f bin` image has no BSS, so these are explicitly zero-initialised
; (`resb` here would warn about uninitialised space in .text). They live within
; the 16 sectors Stage 1 loads, so they are real, writable, zeroed memory.
e820_count: dw 0
vendor_buf: times 16     db 0
e820_buf:   times 24 * 32 db 0
