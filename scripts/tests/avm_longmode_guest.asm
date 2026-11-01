; 64-bit guest for test_avm_longmode_boot.py.  The VMM starts it in long mode
; with paging on, so the checks go through the real CPU paths:
;   - RDMSR and WRMSR of EFER, which the VMM set to LME|LMA
;   - the GS base, which the VMM set to 0x12340000
;   - swapgs, which exchanges GS base with KernelGSBase (VMM value 0x56780000)
;   - WRMSR of LSTAR and STAR, then SYSCALL, which uses the hardware LSTAR
; Each check prints P on pass and F on failure.  The SYSCALL target prints S.
bits 64
org 0x1000

%macro emit 1
    mov dx, 0x3f8
    mov al, %1
    out dx, al
%endmacro

start:
    ; EFER must show LME and LMA.
    mov ecx, 0xC0000080
    rdmsr
    and eax, 0x500
    cmp eax, 0x500
    jne fail
    emit 'P'

    ; WRMSR EFER with SCE set, then read it back.
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1
    wrmsr
    mov ecx, 0xC0000080
    rdmsr
    test eax, 1
    jz fail
    emit 'P'

    ; GS base: the VMM set it to 0x12340000.
    mov ecx, 0xC0000101
    rdmsr
    shl rdx, 32
    or rax, rdx
    cmp rax, 0x12340000
    jne fail
    emit 'P'

    ; swapgs puts KernelGSBase (0x56780000) into GS base.  Swap back after.
    swapgs
    mov ecx, 0xC0000101
    rdmsr
    shl rdx, 32
    or rax, rdx
    swapgs
    cmp rax, 0x56780000
    jne fail
    emit 'P'

    ; LSTAR is this page's handler.  STAR[47:32] = 0x08 is the SYSCALL CS.
    mov ecx, 0xC0000082
    mov rax, handler
    mov rdx, rax
    shr rdx, 32
    wrmsr
    mov ecx, 0xC0000081
    xor eax, eax
    mov edx, 0x00000008
    wrmsr
    emit 'P'
    syscall

fail:
    emit 'F'
    hlt

handler:
    emit 'S'
    hlt
