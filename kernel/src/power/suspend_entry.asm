; SysV x86-64 S3 entry wrapper. The saved stack contains the interrupted
; caller's callee-saved registers and flags; the ACPI wake trampoline resumes
; at .resume with interrupts disabled and the BSP kernel CR3 active.

bits 64

global power_s3_enter_arch
extern power_s3_prepare
extern power_s3_before_sleep
extern power_s3_restore_bsp_early
extern power_s3_cancel
extern power_s3_resume
extern fpu_on_context_switch
extern acpi_enter_s3

section .text
power_s3_enter_arch:
    pushfq
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    lea rsi, [rel .resume]
    call power_s3_prepare
    test eax, eax
    jnz .out

    cli
    call fpu_on_context_switch
    call power_s3_before_sleep
    call acpi_enter_s3
    test eax, eax
    jnz .resume

    ; The platform rejected SLP_EN without entering S3. Restore every state
    ; item we quiesced and return an I/O error to the sysfs writer.
    call power_s3_cancel
    mov eax, -5
    jmp .out

.resume:
    call power_s3_restore_bsp_early
    call power_s3_resume

.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    ret
