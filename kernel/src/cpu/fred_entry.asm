global fred_entry_user
global fred_entry_kernel

extern isr_handler
extern syscall_dispatcher
extern cpu_has_smap_flag

section .text
align 4096

; Ring 3 -> Ring 0 FRED entrypoint (must be 4 KiB aligned).
fred_entry_user:
    push rax
    movzx eax, byte [rsp + 54]   ; SS[55:48] -> low nibble is FRED event type
    and eax, 0x0F
    cmp eax, 7                   ; FRED_EVENT_TYPE_OTHER (SYSCALL / SYSENTER)
    je .is_syscall
    pop rax
    jmp fred_user_event
.is_syscall:
    pop rax
    jmp fred_user_syscall

    times 256 - ($ - fred_entry_user) db 0x90

; Ring 0 -> Ring 0 FRED entrypoint (must be exactly fred_entry_user + 256).
fred_entry_kernel:
    sub rsp, 56                  ; space for ss, rsp, rflags, cs, rip, err_code, int_no
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

    ; Populate struct registers from the 64-byte FRED frame at [rsp + 176].
    movzx eax, byte [rsp + 176 + 44]
    mov [rsp + 120], rax         ; int_no
    mov rax, [rsp + 176 + 0]
    mov [rsp + 128], rax         ; err_code
    mov rax, [rsp + 176 + 8]
    mov [rsp + 136], rax         ; rip
    movzx eax, word [rsp + 176 + 16]
    mov [rsp + 144], rax         ; cs
    mov rax, [rsp + 176 + 24]
    mov [rsp + 152], rax         ; rflags
    mov rax, [rsp + 176 + 32]
    mov [rsp + 160], rax         ; rsp
    movzx eax, word [rsp + 176 + 40]
    mov [rsp + 168], rax         ; ss

    mov rdi, rsp
    mov rbp, rsp
    and rsp, -16
    call isr_handler
    mov rsp, rbp

    ; Write any handler-modified return state back to the FRED frame.
    mov rax, [rsp + 136]
    mov [rsp + 176 + 8], rax     ; rip
    mov ax, [rsp + 144]
    mov word [rsp + 176 + 16], ax ; cs selector (preserve CSL/upper bits)
    mov rax, [rsp + 152]
    mov [rsp + 176 + 24], rax    ; rflags
    mov rax, [rsp + 160]
    mov [rsp + 176 + 32], rax    ; rsp
    mov ax, [rsp + 168]
    mov word [rsp + 176 + 40], ax ; ss selector (preserve upper bits)

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
    add rsp, 56
    erets

; Ring 3 -> Ring 0 non-syscall event (IRQ, NMI, exception, INTn).
fred_user_event:
    sub rsp, 56
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

    movzx eax, byte [rsp + 176 + 44]
    mov [rsp + 120], rax         ; int_no
    mov rax, [rsp + 176 + 0]
    mov [rsp + 128], rax         ; err_code
    mov rax, [rsp + 176 + 8]
    mov [rsp + 136], rax         ; rip
    movzx eax, word [rsp + 176 + 16]
    mov [rsp + 144], rax         ; cs
    mov rax, [rsp + 176 + 24]
    mov [rsp + 152], rax         ; rflags
    mov rax, [rsp + 176 + 32]
    mov [rsp + 160], rax         ; rsp
    movzx eax, word [rsp + 176 + 40]
    mov [rsp + 168], rax         ; ss

    mov rdi, rsp
    mov rbp, rsp
    and rsp, -16
    call isr_handler
    mov rsp, rbp

    mov rax, [rsp + 136]
    mov [rsp + 176 + 8], rax     ; rip
    mov ax, [rsp + 144]
    mov word [rsp + 176 + 16], ax
    mov rax, [rsp + 152]
    mov [rsp + 176 + 24], rax    ; rflags
    mov rax, [rsp + 160]
    mov [rsp + 176 + 32], rax    ; rsp
    mov ax, [rsp + 168]
    mov word [rsp + 176 + 40], ax

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
    add rsp, 56
    eretu

; Ring 3 -> Ring 0 SYSCALL under FRED.
; Builds struct syscall_regs (160 bytes) on top of the 64-byte FRED frame.
fred_user_syscall:
    push rdi                     ; offset 152: user_rdi
    push rsi                     ; offset 144: user_rsi
    push rcx                     ; offset 136: user_rcx
    movzx ecx, word [rsp + 24 + 16]
    push rcx                     ; offset 128: user_cs (0x2b)
    push qword [rsp + 32 + 32]   ; offset 120: rsp
    push qword [rsp + 40 + 24]   ; offset 112: rflags
    push qword [rsp + 48 + 8]    ; offset 104: rip
    push r15                     ; 96
    push r14                     ; 88
    push r13                     ; 80
    push r12                     ; 72
    push rbp                     ; 64
    push rbx                     ; 56
    push rax                     ; 48
    push r9                      ; 40
    push r8                      ; 32
    push r10                     ; 24
    push rdx                     ; 16
    push rsi                     ; 8
    push rdi                     ; 0

    mov rdi, rsp
    sti

    cmp byte [rel cpu_has_smap_flag], 0
    je .stac_done
    stac
.stac_done:

    call syscall_dispatcher

    cmp byte [rel cpu_has_smap_flag], 0
    je .clac_done
    clac
.clac_done:

    cli

    ; rt_sigreturn sets gs:[384] to a struct registers * frame.
    mov rax, gs:[384]
    test rax, rax
    jnz .sigreturn_eretu

    ; Copy potentially modified rip/rflags/rsp/cs back into the FRED frame.
    mov r11, [rsp + 104]
    mov [rsp + 160 + 8], r11     ; rip
    mov r11, [rsp + 112]
    mov [rsp + 160 + 24], r11    ; rflags
    mov r11, [rsp + 120]
    mov [rsp + 160 + 32], r11    ; rsp
    mov r11w, [rsp + 128]
    mov word [rsp + 160 + 16], r11w ; user_cs

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
    add rsp, 56                  ; drop rip..user_rdi; rsp now at FRED frame
    eretu

.sigreturn_eretu:
    mov qword gs:[384], 0

    ; Build a fresh 64-byte FRED frame from struct registers * in rax.
    push qword 0                 ; reserved
    push qword 0                 ; event_data
    push qword [rax + 168]       ; ss
    push qword [rax + 160]       ; rsp
    push qword [rax + 152]       ; rflags
    push qword [rax + 144]       ; cs
    push qword [rax + 136]       ; rip
    push qword 0                 ; error_code

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
    eretu
