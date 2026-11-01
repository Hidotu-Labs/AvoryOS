; Real-mode guest for the AVM PIC/PIT test.  Assembled with nasm -f bin and
; loaded at guest-physical 0x1000.
;
; It remaps the 8259 pair (master base 0x20, slave base 0x28), unmasks IRQ 0,
; and starts PIT channel 0 in rate-generator mode (reload 11932, about 10 ms).
; It halts until five ticks have arrived, latches the counter to check that it
; is counting down, then writes 'L' or 'H', the tick count digit, and 'K' to
; COM1.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x3000
    mov word [0x2000], 0            ; tick counter
    mov word [0x80], handler        ; IVT entry for vector 0x20: offset
    mov word [0x82], 0              ; ... and segment

    ; Master PIC: ICW1 (edge, cascade, ICW4), ICW2 base 0x20, ICW3 slave on
    ; IRQ 2, ICW4 (8086 mode).
    mov al, 0x11
    out 0x20, al
    mov al, 0x20
    out 0x21, al
    mov al, 0x04
    out 0x21, al
    mov al, 0x01
    out 0x21, al

    ; Slave PIC: base 0x28, cascade identity 2, 8086 mode.  All masked.
    mov al, 0x11
    out 0xA0, al
    mov al, 0x28
    out 0xA1, al
    mov al, 0x02
    out 0xA1, al
    mov al, 0x01
    out 0xA1, al
    mov al, 0xFF
    out 0xA1, al

    mov al, 0xFE                    ; master: unmask IRQ 0 only
    out 0x21, al

    ; PIT channel 0: lo/hi access, mode 2 (rate generator), binary.
    mov al, 0x34
    out 0x43, al
    mov al, 0x9C                    ; reload 0x2E9C = 11932
    out 0x40, al
    mov al, 0x2E
    out 0x40, al

    sti
wait_ticks:
    hlt
    cmp word [0x2000], 5
    jb wait_ticks

    cli
    ; Latch channel 0 and read it back: a counting PIT is below its reload.
    mov al, 0x00
    out 0x43, al
    in al, 0x40
    mov bl, al
    in al, 0x40
    mov bh, al
    mov al, 'H'
    cmp bx, 11932
    jae report
    mov al, 'L'
report:
    mov dx, 0x3F8
    out dx, al
    mov al, [0x2000]
    add al, '0'
    out dx, al
    mov al, 'K'
    out dx, al
    hlt

handler:
    inc word [0x2000]
    push ax
    mov al, 0x20
    out 0x20, al                    ; non-specific EOI on the master
    pop ax
    iret
