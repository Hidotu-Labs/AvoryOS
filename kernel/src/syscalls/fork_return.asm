global fork_return_to_userspace
extern cpu_has_fred_flag
extern cpu_md_clear_active
extern cpu_verw_ds

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
    mov  ax, 0x23           ; User Data selector (0x20 | RPL=3)
    mov  ds, ax
    mov  es, ax
    pop  rax

    cmp byte [rel cpu_has_fred_flag], 0
    jne .fred_eretu

    cmp qword [rax + 128], 0x1b
    je .compat_iret

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
    mov r11, 0x00007FFFFFFFFFFF
    cmp rcx, r11
    jbe .sysret_rip_ok
    xor ecx, ecx
.sysret_rip_ok:
    mov r11, [rax + 112]    ; R11 = user RFLAGS (sysret loads this)

    ; Mask interrupts BEFORE the user RSP is loaded.  Interrupts may be
    ; enabled while the child unwinds; after `mov rsp, ...` the CPU is still
    ; ring 0 but RSP is the user stack, and an ISR landing in that window
    ; pushes its frame onto user memory (SMAP fault -> nested fault -> #DF).
    ; Same window syscall_entry protects on its way out.
    cli

    ; Load user RSP
    mov rsp, [rax + 120]

    ; Load rdi (was our struct pointer, now restore user's value)
    mov rdi, [rax + 0]

    ; Load rax last — child fork return value (0)
    mov rax, [rax + 48]

    ; Swap GS: put user GS (0) into active, save kernel GS base
    swapgs

    cmp byte [rel cpu_md_clear_active], 0
    je .skip_verw_fork_sysret
    verw word [rel cpu_verw_ds]
.skip_verw_fork_sysret:

    ; Return to user mode
    o64 sysret

.compat_iret:
    ; Compatibility mode cannot return through SYSRETQ. Keep the interrupt
    ; frame on the kernel stack until IRETQ switches both privilege and mode.
    cli
    push qword 0x23
    push qword [rax + 120]
    push qword [rax + 112]
    push qword 0x1b
    push qword [rax + 104]
    mov r15, [rax + 96]
    mov r14, [rax + 88]
    mov r13, [rax + 80]
    mov r12, [rax + 72]
    xor r11d, r11d
    xor r10d, r10d
    xor r9d, r9d
    xor r8d, r8d
    mov rbp, [rax + 64]
    mov rbx, [rax + 56]
    mov rdx, [rax + 16]
    mov rcx, [rax + 136]
    mov rsi, [rax + 144]
    mov rdi, [rax + 152]
    mov rax, [rax + 48]
    swapgs
    cmp byte [rel cpu_md_clear_active], 0
    je .skip_verw_fork_iret
    verw word [rel cpu_verw_ds]
.skip_verw_fork_iret:
    iretq

.fred_eretu:
    cli
    push qword 0                 ; reserved
    push qword 0                 ; event_data
    push qword 0x23              ; user SS
    push qword [rax + 120]       ; user RSP
    push qword [rax + 112]       ; user RFLAGS
    push qword [rax + 128]       ; user CS (0x2b or 0x1b)
    push qword [rax + 104]       ; user RIP
    push qword 0                 ; error_code

    mov r15, [rax + 96]
    mov r14, [rax + 88]
    mov r13, [rax + 80]
    mov r12, [rax + 72]
    mov rbp, [rax + 64]
    mov rbx, [rax + 56]
    mov r9,  [rax + 40]
    mov r8,  [rax + 32]
    mov r10, [rax + 24]
    mov rdx, [rax + 16]
    cmp qword [rax + 128], 0x1b
    je .fred_compat_regs
    mov rsi, [rax + 8]
    mov rdi, [rax + 0]
    mov rcx, [rax + 104]
    mov r11, [rax + 112]
    mov rax, [rax + 48]
    cmp byte [rel cpu_md_clear_active], 0
    je .skip_verw_fork_fred64
    verw word [rel cpu_verw_ds]
.skip_verw_fork_fred64:
    eretu

.fred_compat_regs:
    xor r11d, r11d
    xor r10d, r10d
    xor r9d, r9d
    xor r8d, r8d
    mov rcx, [rax + 136]
    mov rsi, [rax + 144]
    mov rdi, [rax + 152]
    mov rax, [rax + 48]
    cmp byte [rel cpu_md_clear_active], 0
    je .skip_verw_fork_fred32
    verw word [rel cpu_verw_ds]
.skip_verw_fork_fred32:
    eretu
