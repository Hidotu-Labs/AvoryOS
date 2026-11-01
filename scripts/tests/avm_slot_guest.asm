; Real-mode guest for the AVM memslot-deletion test.  Assembled with nasm -f bin
; and loaded at guest-physical 0x1000.
;
; It writes 'R', then reads the word at 0x3000 in a loop with no exits.  While
; it runs, the VMM removes the memslot that backs 0x3000 from another thread.
; The next read must reach the VMM (an MMIO exit), which supplies 0x77, and
; the guest writes 'M'.  If the loop runs out without seeing 0x77, it writes
; 'T' instead.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x2800
    mov word [0x3000], 0

    mov al, 'R'
    call putc

    mov bp, 3000                    ; outer loop
outer:
    mov cx, 0                       ; inner loop: 65536 reads
inner:
    mov ax, [0x3000]
    cmp ax, 0x77
    je seen
    dec cx
    jnz inner
    dec bp
    jnz outer

    mov al, 'T'
    call putc
    hlt

seen:
    mov al, 'M'
    call putc
    hlt

putc:
    mov dx, 0x3F8
    out dx, al
    ret
