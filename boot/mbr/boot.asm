; boot/mbr/boot.asm
; ============================================================================
; PRADYOS-BOOT — Stage 1 (legacy BIOS MBR loader), Phase 1.
;
; The BIOS loads this 512-byte sector to 0x0000:0x7C00 in 16-bit real mode and
; jumps to it (signature 0xAA55 required at offset 510). Stage 1's only job is
; to load Stage 2 from disk and jump to it — Stage 2 is where A20, the E820
; memory map, CPUID detection, and the protected-mode switch live (those don't
; fit in 512 bytes).
;
; Disk layout produced by the Makefile:
;   LBA 0      : this sector (Stage 1)
;   LBA 1..16  : Stage 2 (build/stage2.bin, <= 8 KiB), loaded to 0x0000:0x7E00
;
; We read with INT 13h AH=42h (extended/LBA read) via a Disk Address Packet,
; which is supported by all modern BIOSes and QEMU's SeaBIOS, and avoids
; CHS geometry guesswork.
;
; References (verify, do not trust from memory):
;   - OSDev Wiki "Disk access using the BIOS (INT 13h)" — AH=42h DAP layout.
;   - OSDev Wiki "Serial Ports" — 16550 register map (COM1=0x3F8).
;   - Ralf Brown's Interrupt List — INT 13h/AH=42h, INT 10h/AH=0Eh.
;
; Build: nasm -f bin boot.asm -o stage1.bin   (exactly 512 bytes)
; ============================================================================

BITS 16
ORG 0x7C00

COM1 equ 0x3F8

stage1:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; stack below us; Stage 2 loads above at 0x7E00
    sti
    mov [boot_drive], dl        ; BIOS leaves the boot drive number in DL

    call serial_init
    mov si, msg_s1
    call puts

    mov si, dap                 ; DS:SI -> Disk Address Packet
    mov ah, 0x42                ; extended read
    mov dl, [boot_drive]
    int 0x13
    jc .disk_err

    mov dl, [boot_drive]        ; pass boot drive to Stage 2 in DL (INT 13h clobbers it)
    jmp 0x0000:0x7E00           ; hand off to Stage 2 (sets CS=0)

.disk_err:
    mov si, msg_err
    call puts
.halt:
    hlt
    jmp .halt

; --- COM1 init: 115200 baud, 8N1, FIFO on. (See OSDev "Serial Ports".) -------
serial_init:
    mov dx, COM1 + 1
    mov al, 0x00
    out dx, al                 ; IER: interrupts off
    mov dx, COM1 + 3
    mov al, 0x80
    out dx, al                 ; LCR: DLAB on
    mov dx, COM1 + 0
    mov al, 0x01
    out dx, al                 ; divisor LSB = 1 (115200)
    mov dx, COM1 + 1
    mov al, 0x00
    out dx, al                 ; divisor MSB = 0
    mov dx, COM1 + 3
    mov al, 0x03
    out dx, al                 ; LCR: 8N1, DLAB off
    mov dx, COM1 + 2
    mov al, 0xC7
    out dx, al                 ; FCR: FIFO enable + clear
    mov dx, COM1 + 4
    mov al, 0x0B
    out dx, al                 ; MCR: DTR+RTS+OUT2
    ret

serial_putc:                   ; AL = byte; preserves AX, DX
    push dx
    push ax
    mov ah, al
.w:
    mov dx, COM1 + 5
    in al, dx
    test al, 0x20              ; THRE?
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

puts:                          ; DS:SI -> NUL-terminated string
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

boot_drive db 0
msg_s1     db "PRADYOS S1: loading stage2...", 13, 10, 0
msg_err    db "PRADYOS S1: DISK READ ERROR", 13, 10, 0

; Disk Address Packet for INT 13h/AH=42h.
dap:
    db 0x10                    ; packet size
    db 0x00                    ; reserved
    dw 16                      ; sectors to read (8 KiB; Stage 2 fits in this)
    dw 0x7E00                  ; destination offset
    dw 0x0000                  ; destination segment
    dq 1                       ; starting LBA (Stage 2 begins right after MBR)

times 510 - ($ - $$) db 0
dw 0xAA55
