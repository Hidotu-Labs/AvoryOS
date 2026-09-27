global switch_context
global thread_stub

section .text

extern cpu_has_xsave_flag

; void switch_context(struct thread *old_t, struct thread *new_t)
; rdi = pointer to old thread struct
; rsi = pointer to new thread struct
switch_context:
    ; Save RFLAGS with the rest of the context.  RFLAGS.AC is the coarse SMAP
    ; user-access window: a thread that blocks mid-syscall must resume with AC
    ; still set, and a thread resumed from user/idle context must not inherit
    ; somebody else's window.
    pushfq

    ; Push callee-saved registers according to System V AMD64 ABI
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Save FPU/XSAVE state (at offset 64 in struct thread)
    cmp byte [rel cpu_has_xsave_flag], 0
    je .save_fxsave

    mov eax, 0xFFFFFFFF
    mov edx, 0xFFFFFFFF
    xsave64 [rdi + 64]
    jmp .fpu_saved

.save_fxsave:
    fxsave64 [rdi + 64]

.fpu_saved:
    ; Save current stack pointer into old_t->rsp (offset 0)
    mov [rdi], rsp

    ; Load new stack pointer from new_t->rsp (offset 0)
    mov rsp, [rsi]

    ; The old kernel stack is no longer in use. Clear this actual CPU's
    ; stack hazard here; a resumed C frame may hold a stale CPU pointer.
    mov qword [gs:376], 0

    ; Restore FPU/XSAVE state from new thread (at offset 64 in struct thread)
    cmp byte [rel cpu_has_xsave_flag], 0
    je .restore_fxrstor

    mov eax, 0xFFFFFFFF
    mov edx, 0xFFFFFFFF
    xrstor64 [rsi + 64]
    jmp .fpu_restored

.restore_fxrstor:
    fxrstor64 [rsi + 64]

.fpu_restored:
    ; Pop callee-saved registers for the arriving thread
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Restore this thread's RFLAGS (including the AC window) before returning.
    ; IF is explicitly cleared again: the scheduler invariant is that a resumed
    ; thread runs with interrupts masked until sched_schedule() reaches its
    ; hal_irq_enable(), and letting popfq re-enable IF here allowed an
    ; interrupt to nest a second schedule while the first one was still
    ; unwinding (random corrupted returns).  AC must survive - it is the
    ; coarse SMAP window for a syscall that blocked mid-dispatch.
    popfq
    cli

    ; Return to the address left on the new thread's stack
    ret

; This stub is where newly created threads begin execution.
; The switch_context "ret" instruction pops into here.
; 'r12' contains the actual C function entry point (set in sched_create_kernel_thread).
thread_stub:
    ; Ensure interrupts are enabled for the new thread
    sti

    ; Call the entry function
    call r12

    ; If the entry function returns, it will return into thread_exit()
    ; because thread_exit was pushed just below the context struct.
    ret
