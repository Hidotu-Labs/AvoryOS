; Real-mode guest for the AVM vCPU migration test.  Assembled with nasm -f bin and
; loaded at guest-physical 0x1000.
;
; It keeps a value in BX and spins for a long time.  Every outer iteration it
; writes '.' to COM1, so the vCPU returns to the VMM often and the thread can be
; moved between host CPUs.  At the end BX must still hold its value; then it
; writes 'K', or 'X' if BX changed.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x2800
    mov bx, 0x1234

    mov bp, 4000
outer:
    mov al, '.'
    call putc
    mov cx, 0
inner:
    dec cx
    jnz inner
    dec bp
    jnz outer

    cmp bx, 0x1234
    jne bad
    mov al, 'K'
    call putc
    hlt

bad:
    mov al, 'X'
    call putc
    hlt

putc:
    mov dx, 0x3F8
    out dx, al
    ret
