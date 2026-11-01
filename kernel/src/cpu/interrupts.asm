extern isr_handler

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push %1
    jmp isr_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
; NMI.  An NMI can arrive in the syscall exit window, after the swapgs that
; precedes sysret and before the sysret itself.  There CS is still the kernel's,
; so the common stub would not swap, and the handler would read per-CPU data
; through the user's GS base.  This entry checks the GS base in that case, swaps,
; and sets NMI_GS_MARK in the error-code slot.  The common exit swaps back when it
; sees the mark.  An NMI from user mode is left to the common stub.
NMI_GS_MARK equ 0x4E4D4953
global isr2
isr2:
    push 0                  ; error-code slot, set to the mark below if needed
    push 2                  ; interrupt number
    push rax
    push rcx
    push rdx
    test qword [rsp + 48], 3    ; CS of the NMI frame
    jnz .nmi_done
    mov ecx, 0xC0000101         ; IA32_GS_BASE
    rdmsr
    test edx, edx
    js .nmi_done                ; GS base is already the kernel's
    swapgs
    mov qword [rsp + 32], NMI_GS_MARK
.nmi_done:
    pop rdx
    pop rcx
    pop rax
    jmp isr_common_stub
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

; IRQs
ISR_NOERRCODE 32
ISR_NOERRCODE 33
ISR_NOERRCODE 34
ISR_NOERRCODE 35
ISR_NOERRCODE 36
ISR_NOERRCODE 37
ISR_NOERRCODE 38
ISR_NOERRCODE 39
ISR_NOERRCODE 40
ISR_NOERRCODE 41
ISR_NOERRCODE 42
ISR_NOERRCODE 43
ISR_NOERRCODE 44
ISR_NOERRCODE 45
ISR_NOERRCODE 46
ISR_NOERRCODE 47

; LAPIC timer interrupt vector
ISR_NOERRCODE 48

; Reschedule IPI vector
ISR_NOERRCODE 49

; HPET watchdog vector
ISR_NOERRCODE 50

; Dynamically allocated MSI/MSI-X device vectors.
%assign dynamic_vector 51
%rep 204
ISR_NOERRCODE dynamic_vector
%assign dynamic_vector dynamic_vector+1
%endrep

section .rodata
global isr_dynamic_stub_table
isr_dynamic_stub_table:
%assign dynamic_vector 51
%rep 204
    dq isr%+dynamic_vector
%assign dynamic_vector dynamic_vector+1
%endrep

section .text

; LAPIC spurious interrupt vector
ISR_NOERRCODE 255

isr_common_stub:
    ; Stack at entry:
    ;   [rsp+0]  = int_no   (pushed by ISR macro)
    ;   [rsp+8]  = err_code (pushed by CPU or macro)
    ;   [rsp+16] = rip      (CPU)
    ;   [rsp+24] = cs       (CPU)
    ;   [rsp+32] = rflags   (CPU)
    ;   [rsp+40] = rsp      (interrupted stack, CPU)
    ;   [rsp+48] = ss       (CPU)
    ; In 64-bit mode SS:RSP are pushed even without a privilege change.
    ; The saved RSP also preserves the pointer before interrupt alignment.

    ; The kernel is entirely responsible for the direction flag: memset and
    ; memcpy here are REP strings, so an interrupted context with RFLAGS.DF=1
    ; would make every one of them copy backwards through memory. Clearing it
    ; is safe for the interrupted code because iretq restores RFLAGS from the
    ; frame the CPU pushed before this instruction.
    cld

    test qword [rsp + 24], 3    ; check CPL bits in saved CS
    jz .skip_swapgs
    swapgs
.skip_swapgs:

    ; Push all general purpose registers (matching struct registers)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Call C handler
    mov rdi, rsp           ; First parameter: pointer to struct registers
    mov rbp, rsp           ; Save RSP for restoring after alignment
    and rsp, -16           ; Align stack to 16 bytes for System V AMD64 ABI
    call isr_handler
    mov rsp, rbp           ; Restore RSP

    ; Pop all general purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Swap GS back if returning to user mode
    test qword [rsp + 24], 3
    jz .skip_swapgs_exit
    swapgs
.skip_swapgs_exit:

    ; An NMI that swapped GS on entry swaps back here.
    cmp qword [rsp + 8], NMI_GS_MARK
    jne .skip_nmi_gs_exit
    swapgs
.skip_nmi_gs_exit:

    add rsp, 16             ; remove int_no + err_code
    iretq
