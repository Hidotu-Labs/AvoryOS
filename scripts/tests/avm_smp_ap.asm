; Application processor for the AVM SMP test.  Assembled with nasm -f bin and
; loaded at guest-physical 0x8000, where SIPI vector 0x08 starts it.
;
; It writes 'A' to COM1, sends a fixed IPI on vector 0x40 to APIC ID 0, and
; halts.

bits 16
org 0x8000

ap_start:
    mov dx, 0x3F8
    mov al, 'A'
    out dx, al

    mov ecx, 0x830                  ; ICR: fixed IPI, vector 0x40, to APIC ID 0
    mov eax, 0x4040
    xor edx, edx
    wrmsr

    hlt
