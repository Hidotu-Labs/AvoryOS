; Real-mode guest for the AVM per-VM ASID test.  Assembled with nasm -f bin and
; loaded at guest-physical 0x1000.
;
; BX holds a marker set by the VMM, different for each VM.  The guest stores it
; at 0x3000 and checks, for a long loop, that its own value stays there.  Then
; it writes 'V', the marker byte, and 'K'.  A wrong value writes 'X'.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x2800
    mov [0x3000], bx

    mov bp, 300
outer:
    mov cx, 0
inner:
    cmp [0x3000], bx
    jne bad
    dec cx
    jnz inner
    dec bp
    jnz outer

    mov al, 'V'
    call putc
    mov al, bl
    call putc
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
