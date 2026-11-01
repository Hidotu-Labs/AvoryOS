; Real-mode guest for the AVM guest MSR test, loaded at 0x1000 with paging off.
; It uses RDMSR and WRMSR itself, not the VMM's ioctls.  MTRRCAP (0xFE) reads
; as 0x508.  MTRRdefType (0x2FF) takes a write and reads it back.  The result
; goes to COM1 as "OK", or "N" for the first check that fails.

bits 16
org 0x1000

start:
    mov ecx, 0xFE           ; MTRRCAP is read-only; it reads 0x508
    rdmsr
    cmp eax, 0x508
    jne bad
    cmp edx, 0
    jne bad

    mov ecx, 0x2FF          ; MTRRdefType: write 0xC00, read it back
    mov eax, 0xC00
    xor edx, edx
    wrmsr
    mov ecx, 0x2FF
    rdmsr
    cmp eax, 0xC00
    jne bad

    mov dx, 0x3F8
    mov al, 'O'
    out dx, al
    mov al, 'K'
    out dx, al
    hlt

bad:
    mov dx, 0x3F8
    mov al, 'N'
    out dx, al
    hlt
