; Real-mode guest for the AVM busy-timer test.  Assembled with nasm -f bin and
; loaded at guest-physical 0x1000.
;
; It starts the x2APIC timer (16 ms ticks) and then spins without halting or
; touching a device, so it exits only if the host forces it.  After three
; ticks it writes "B<ticks>K" to COM1.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x3000
    mov word [0x2000], 0
    mov word [0x80], handler
    mov word [0x82], 0

    mov ecx, 0x80F                 ; SVR: software enable
    mov eax, 0x1FF
    xor edx, edx
    wrmsr

    mov ecx, 0x83E                 ; divide by 16
    mov eax, 0x3
    xor edx, edx
    wrmsr

    mov ecx, 0x832                 ; periodic, vector 0x20
    mov eax, 0x20020
    xor edx, edx
    wrmsr

    mov ecx, 0x838                 ; 1000 counts = 16 ms
    mov eax, 1000
    xor edx, edx
    wrmsr

    sti
busy:
    cmp word [0x2000], 3
    jb busy

    cli
    mov dx, 0x3F8
    mov al, 'B'
    out dx, al
    mov al, [0x2000]
    add al, '0'
    out dx, al
    mov al, 'K'
    out dx, al
    hlt

handler:
    inc word [0x2000]
    push eax
    push ecx
    push edx
    mov ecx, 0x80B                 ; EOI
    xor eax, eax
    xor edx, edx
    wrmsr
    pop edx
    pop ecx
    pop eax
    iret
