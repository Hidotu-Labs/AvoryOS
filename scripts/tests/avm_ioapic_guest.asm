; Real-mode guest for the AVM APIC/IOAPIC/MMIO test.  Assembled with nasm -f bin
; and loaded at guest-physical 0x1000.
;
; It uses xAPIC MMIO (a32 prefixes give 32-bit addresses in real mode):
;   - reads the APIC ID (must be 0),
;   - programs the LAPIC timer, and sets up IOAPIC pin 16 with vector 0x31,
;   - reads an unmapped address, which the VMM answers with 0x5A,
;   - writes 'R', so the VMM raises IOAPIC pin 16,
; then halts until it has seen 3 timer ticks and 1 IOAPIC interrupt.  It
; writes "T<ticks>O<ioapic>K" to COM1.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x3000
    mov word [0x2000], 0           ; LAPIC timer ticks
    mov word [0x2002], 0           ; IOAPIC interrupts
    mov word [0x80], timer_handler ; IVT 0x20: LAPIC timer
    mov word [0x82], 0
    mov word [0xC4], io_handler    ; IVT 0x31: IOAPIC pin 16
    mov word [0xC6], 0

    ; Software-enable the LAPIC: SVR with spurious vector 0xFF.
    a32 mov dword [0xFEE000F0], 0x1FF

    ; The APIC ID of the only vCPU is 0.
    a32 mov eax, [0xFEE00020]
    test eax, eax
    mov al, 'i'                     ; mov does not change the flags
    jnz .id_bad
    mov al, 'I'
.id_bad:
    call putc

    ; Timer: divide by 16, periodic on vector 0x20, 1000 counts (16 ms).
    a32 mov dword [0xFEE003E0], 3
    a32 mov dword [0xFEE00320], 0x20020
    a32 mov dword [0xFEE00380], 1000

    ; IOAPIC pin 16 (redirection entry at index 0x30/0x31): vector 0x31,
    ; fixed, edge, unmasked, destination APIC 0.
    a32 mov dword [0xFEC00000], 0x30
    a32 mov dword [0xFEC00010], 0x31
    a32 mov dword [0xFEC00000], 0x31
    a32 mov dword [0xFEC00010], 0

    ; Unmapped MMIO: the VMM supplies the value 0x5A.
    a32 mov eax, [0xD0000000]
    cmp al, 0x5A
    mov al, 'm'
    jne .mmio_bad
    mov al, 'M'
.mmio_bad:
    call putc

    ; Ask the VMM to raise IOAPIC pin 16.
    mov al, 'R'
    call putc

    sti
wait_irqs:
    hlt
    cmp word [0x2002], 1
    jb wait_irqs
    cmp word [0x2000], 3
    jb wait_irqs

    cli
    mov al, 'T'
    call putc
    mov al, [0x2000]
    add al, '0'
    call putc
    mov al, 'O'
    call putc
    mov al, [0x2002]
    add al, '0'
    call putc
    mov al, 'K'
    call putc
    hlt

putc:
    mov dx, 0x3F8
    out dx, al
    ret

timer_handler:
    inc word [0x2000]
    push eax
    a32 mov dword [0xFEE000B0], 0  ; EOI
    pop eax
    iret

io_handler:
    inc word [0x2002]
    push eax
    a32 mov dword [0xFEE000B0], 0  ; EOI
    pop eax
    iret
