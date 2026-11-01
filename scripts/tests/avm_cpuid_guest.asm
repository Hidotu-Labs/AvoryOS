; Real-mode guest for the AVM CPUID test.  Assembled with nasm -f bin and loaded
; at guest-physical 0x1000.
;
; The VMM installs a CPUID table with one leaf, 0x40000000, whose EBX is "TEST",
; and no leaf 1.  The guest reads both.  Expected output: 'T' (the VMM's table
; is used), 'Z' (leaf 1 is absent, so it reads as zero), then 'K'.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov sp, 0x3000

    mov eax, 0x40000000
    cpuid
    cmp ebx, 'TEST'
    jne .wrong_hv
    mov al, 'T'
    jmp .hv_done
.wrong_hv:
    mov al, 'N'
.hv_done:
    call putc

    mov eax, 1
    cpuid
    test ecx, ecx
    jnz .leaf1_present
    mov al, 'Z'
    jmp .leaf1_done
.leaf1_present:
    mov al, 'F'
.leaf1_done:
    call putc

    mov al, 'K'
    call putc
    hlt

putc:
    mov dx, 0x3F8
    out dx, al
    ret
