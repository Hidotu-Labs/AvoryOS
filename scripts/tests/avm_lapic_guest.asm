; Real-mode guest for the AVM LAPIC test.  Assembled with nasm -f bin and
; loaded at guest-physical 0x1000.
;
; It switches the local APIC to x2APIC, starts a periodic timer on vector
; 0x20 (16 ms per tick), and halts until five ticks have arrived.  It then
; writes the tick count as a digit and 'K' to COM1.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x3000                  ; stack inside the mapped guest pages
    mov word [0x80], handler        ; IVT entry for vector 0x20: offset
    mov word [0x82], 0              ; ... and segment
    mov word [0x2000], 0            ; tick counter

    mov ecx, 0x1B                   ; IA32_APIC_BASE: set EXTD (x2APIC mode)
    rdmsr
    or eax, 0x400
    wrmsr

    mov ecx, 0x80F                  ; SVR: software enable, spurious 0xFF
    mov eax, 0x1FF
    xor edx, edx
    wrmsr

    mov ecx, 0x83E                  ; divide configuration: divide by 16
    mov eax, 0x3
    xor edx, edx
    wrmsr

    mov ecx, 0x832                  ; LVT timer: periodic (bit 17), vector 0x20
    mov eax, 0x20020
    xor edx, edx
    wrmsr

    mov ecx, 0x838                  ; initial count 1000: 1000 x 16 us = 16 ms
    mov eax, 1000
    xor edx, edx
    wrmsr

    sti
wait_ticks:
    hlt                             ; wakes on each timer interrupt
    cmp word [0x2000], 5
    jb wait_ticks

    cli
    mov ecx, 0x838                  ; stop the timer: initial count 0
    xor eax, eax
    xor edx, edx
    wrmsr

    mov dx, 0x3F8
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
    mov ecx, 0x80B                  ; EOI
    xor eax, eax
    xor edx, edx
    wrmsr
    pop edx
    pop ecx
    pop eax
    iret
