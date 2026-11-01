; Real-mode guest for the AVM XSETBV test.  Assembled with nasm -f bin and loaded
; at guest-physical 0x1000.
;
; It sets CR4.OSXSAVE, then reads XCR0 with XGETBV.  Setting XCR0 to the same
; value must succeed ('V').  Setting it to a subset (SSE dropped) must raise #GP,
; as must an invalid value (AVX without SSE).  The #GP handler reports each fault
; ('G') and skips the 3-byte XSETBV.  Finally it writes 'K' and halts.

bits 16
org 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x3000
    mov word [0x34], gp_handler     ; IVT entry for vector 13 (#GP)
    mov word [0x36], 0

    mov eax, cr4
    or eax, 0x40000                 ; CR4.OSXSAVE (bit 18)
    mov cr4, eax

    xor ecx, ecx
    db 0x0F, 0x01, 0xD0             ; XGETBV: edx:eax = XCR0
    mov ebx, eax                    ; keep the host's XCR0 in EBX

    mov eax, ebx                    ; the same value: must be accepted
    xor edx, edx
    xor ecx, ecx
    db 0x0F, 0x01, 0xD1             ; XSETBV
    mov al, 'V'
    call putc

    mov eax, ebx                    ; a subset (SSE dropped): must fault
    and eax, 0xFFFFFFFD
    xor edx, edx
    xor ecx, ecx
    db 0x0F, 0x01, 0xD1             ; XSETBV, faults and is skipped by the handler

    mov eax, 4                      ; AVX without SSE: must fault
    xor edx, edx
    xor ecx, ecx
    db 0x0F, 0x01, 0xD1             ; XSETBV, faults and is skipped by the handler

    mov al, 'K'
    call putc
    hlt

putc:
    mov dx, 0x3F8
    out dx, al
    ret

gp_handler:
    push bp
    mov bp, sp
    add word [bp + 2], 3            ; the saved IP is just above BP: skip the XSETBV
    pop bp
    mov al, 'G'
    call putc
    iret
