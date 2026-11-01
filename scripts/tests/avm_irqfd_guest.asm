; Real-mode guest for the AVM irqfd test.  Assembled with nasm -f bin and loaded
; at guest-physical 0x1000.
;
; The VMM routes GSI 5 to MSI vector 0x50, and it sends MSI vector 0x51 itself.
; The guest enables its LAPIC, takes both vectors (0x50 writes 'I', 0x51 writes
; 'M'), and halts with interrupts on until both have run.  Then it writes 'K'.
; Each handler clears its ISR bit with an x2APIC EOI.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x3000
    mov word [0x50 * 4], vector_50       ; IVT entry for vector 0x50
    mov word [0x50 * 4 + 2], 0
    mov word [0x51 * 4], vector_51       ; IVT entry for vector 0x51
    mov word [0x51 * 4 + 2], 0

    mov ecx, 0x80F                       ; x2APIC SVR: software enable
    mov eax, 0x1FF
    xor edx, edx
    wrmsr

    sti
idle_loop:
    hlt                                  ; wakes when a vector is pending
    cmp byte [0x2000], 1
    jne idle_loop
    cmp byte [0x2001], 1
    jne idle_loop

    cli
    mov dx, 0x3F8
    mov al, 'K'
    out dx, al
    hlt

vector_50:
    pusha
    mov byte [0x2000], 1
    mov dx, 0x3F8
    mov al, 'I'
    out dx, al
    call eoi
    popa
    iret

vector_51:
    pusha
    mov byte [0x2001], 1
    mov dx, 0x3F8
    mov al, 'M'
    out dx, al
    call eoi
    popa
    iret

eoi:
    mov ecx, 0x80B
    xor eax, eax
    xor edx, edx
    wrmsr
    ret
