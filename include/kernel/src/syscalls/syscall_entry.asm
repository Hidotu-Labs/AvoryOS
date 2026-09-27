global syscall_entry
extern syscall_dispatcher
extern console_puts
extern cpu_has_smap_flag

section .text

syscall_entry:

    swapgs

    ; User mode owns RFLAGS.DF (any process can set it with pushfq/popfq), and
    ; the kernel runs memset/memcpy as REP strings. Clear it before touching
    ; the kernel stack, or the whole syscall runs with string operations
    ; counting downwards. The user's own DF lives on in R11 and is restored
    ; with it on the way out.
    cld

    mov gs:[368], rsp

    mov rsp, gs:[24]

    push qword gs:[368] ; User RSP
    push r11           ; User RFLAGS
    push rcx           ; User RIP

    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx
    push rax
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi

    mov rbp, rsp
    and rsp, -16

    ; Coarse SMAP user-access window: most syscalls in this kernel still read
    ; and write user memory directly instead of going through copy_*_user, so
    ; set RFLAGS.AC for the dispatch.  IA32_FMASK cleared AC on entry, and
    ; switch_context saves/restores RFLAGS per thread so the window survives
    ; blocking syscalls.  stac is #UD without SMAP, so gate it.
    cmp byte [rel cpu_has_smap_flag], 0
    je .stac_done
    stac
.stac_done:

    mov rdi, rbp
    call syscall_dispatcher

    ; Close the window before returning to user mode.  The user's own AC
    ; (stored in R11) is theirs to keep; sysret restores it.
    cmp byte [rel cpu_has_smap_flag], 0
    je .clac_done
    clac
.clac_done:

    ; rt_sigreturn requires IRETQ to restore user RCX and R11.
    mov rax, gs:[384]
    test rax, rax
    jnz .sigreturn_iret

    mov rsp, rbp

    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    pop rax
    pop rbx
    pop rbp
    pop r12
    pop r13
    pop r14
    pop r15

    pop rcx ; User RIP
    pop r11 ; User RFLAGS
    pop rsp ; User RSP

    ; Swap GS back to User TLS (if any)
    ; Disable interrupts to protect the window between swapgs and sysret,
    ; as the ISR would now see a kernel CS but with user GS.
    cli
    swapgs

    ; Return to user mode safely
    o64 sysret

.sigreturn_iret:
    cli
    mov qword gs:[384], 0

    ; Build SS:RSP:RFLAGS:CS:RIP from struct registers.
    push qword [rax + 168]
    push qword [rax + 160]
    push qword [rax + 152]
    push qword [rax + 144]
    push qword [rax + 136]

    ; Keep RAX as the frame pointer until all other GPRs are restored.
    mov r15, [rax + 0]
    mov r14, [rax + 8]
    mov r13, [rax + 16]
    mov r12, [rax + 24]
    mov r11, [rax + 32]
    mov r10, [rax + 40]
    mov r9,  [rax + 48]
    mov r8,  [rax + 56]
    mov rbp, [rax + 64]
    mov rdi, [rax + 72]
    mov rsi, [rax + 80]
    mov rdx, [rax + 88]
    mov rcx, [rax + 96]
    mov rbx, [rax + 104]
    mov rax, [rax + 112]

    swapgs
    iretq

