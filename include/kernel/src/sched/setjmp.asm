global kernel_setjmp
global kernel_longjmp

section .text

; int kernel_setjmp(uint64_t *buf)
; Saves: rbx, rbp, r12, r13, r14, r15, rsp, rip (return address)
; Returns 0 on initial call, non-zero when restored by longjmp.
kernel_setjmp:
    ; rdi = pointer to jmp_buf (array of 8 uint64_t)
    mov [rdi],      rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    ; Save the stack pointer AFTER the return address was pushed by call
    lea rax, [rsp + 8]
    mov [rdi + 48], rax
    ; Save the return address (top of stack is the caller's RIP)
    mov rax, [rsp]
    mov [rdi + 56], rax
    ; Return 0 (first time)
    xor eax, eax
    ret

; void kernel_longjmp(uint64_t *buf, int val)
; Restores state saved by kernel_setjmp. Makes setjmp appear to return 'val'.
; If val == 0, returns 1 instead (setjmp convention).
kernel_longjmp:
    ; rdi = pointer to jmp_buf
    ; esi = return value
    mov eax, esi
    test eax, eax
    jnz .nonzero
    mov eax, 1
.nonzero:
    mov rbx, [rdi]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    ; Push the saved RIP so 'ret' will jump there
    push qword [rdi + 56]
    ret
