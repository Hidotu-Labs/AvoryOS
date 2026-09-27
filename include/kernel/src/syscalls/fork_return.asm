global fork_return_to_userspace

section .text

; void __attribute__((noreturn))
;   fork_return_to_userspace(struct syscall_regs *regs)
;
; Restores the full user register state from the saved frame and performs
; sysretq to transition the forked child back to Ring 3.
;
; struct syscall_regs layout (offsets in bytes, each 8 bytes):
;    0: rdi     8: rsi    16: rdx    24: r10
;   32: r8     40: r9     48: rax    56: rbx
;   64: rbp    72: r12    80: r13    88: r14
;   96: r15   104: rip   112: rflags 120: rsp

fork_return_to_userspace:
    ; rdi = pointer to struct syscall_regs (C calling convention)
    mov rax, rdi            ; stash regs pointer in rax

    ; Set user data segment selectors
    push rax
    mov  ax, 0x1B           ; User Data selector (0x18 | RPL=3)
    mov  ds, ax
    mov  es, ax
    pop  rax

    ; Restore GPRs (except rdi and rax which we handle last)
    mov rsi, [rax + 8]
    mov rdx, [rax + 16]
    mov r10, [rax + 24]
    mov r8,  [rax + 32]
    mov r9,  [rax + 40]
    mov rbx, [rax + 56]
    mov rbp, [rax + 64]
    mov r12, [rax + 72]
    mov r13, [rax + 80]
    mov r14, [rax + 88]
    mov r15, [rax + 96]

    ; Set up sysret registers
    mov rcx, [rax + 104]    ; RCX = user RIP (sysret jumps here)
    mov r11, [rax + 112]    ; R11 = user RFLAGS (sysret loads this)

    ; Load user RSP
    mov rsp, [rax + 120]

    ; Load rdi (was our struct pointer, now restore user's value)
    mov rdi, [rax + 0]

    ; Load rax last — child fork return value (0)
    mov rax, [rax + 48]

    ; Swap GS: put user GS (0) into active, save kernel GS base
    swapgs

    ; Return to user mode
    o64 sysret
