; Boot processor for the AVM SMP test.  Assembled with nasm -f bin and loaded
; at guest-physical 0x1000.
;
; It enables its LAPIC, sends INIT and then SIPI (vector 0x08, so the AP starts
; at 0x8000) to APIC ID 1, and spins in guest mode until the AP's IPI arrives
; on vector 0x40.  Then it writes "BK" to COM1 and halts.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x3000
    mov word [0x9002], 0            ; set by the IPI handler
    mov word [0x100], ipi_handler   ; IVT entry for vector 0x40
    mov word [0x102], 0

    mov ecx, 0x80F                  ; SVR: software enable
    mov eax, 0x1FF
    xor edx, edx
    wrmsr

    mov ecx, 0x830                  ; ICR: INIT to APIC ID 1, level assert
    mov eax, 0x4500
    mov edx, 1
    wrmsr

    mov ecx, 0x830                  ; ICR: SIPI to APIC ID 1, vector 0x08
    mov eax, 0x4608
    mov edx, 1
    wrmsr

    sti
spin:
    cmp word [0x9002], 0
    je spin

    cli
    mov dx, 0x3F8
    mov al, 'B'
    out dx, al
    mov al, 'K'
    out dx, al
    hlt

ipi_handler:
    mov word [0x9002], 1
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
