; svm_run_vcpu(guest_vmcb_pa, host_vmcb_pa, struct kvm_regs *gprs)
;   rdi = guest VMCB (physical), rsi = host VMCB (VMSAVE/VMLOAD target),
;   rdx = guest registers, in struct kvm_regs layout.
;
; Host interrupts must be masked on entry (RFLAGS.IF clear).  VMRUN covers RIP,
; RSP, RAX, RFLAGS and the control state through the VMCB, but not FS/GS/TR/LDTR
; or the GPRs, so this stub saves and restores those itself.  Guest RAX and RSP
; live in the VMCB and are not handled here.
;
; GIF is not cleared (no CLGI) on purpose: the INTR and NMI intercepts only
; fire while GIF is set, and the VM kicks rely on them.

bits 64
default rel

section .text
global svm_run_vcpu

svm_run_vcpu:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rdx                    ; [rsp+16] = gprs
    push rsi                    ; [rsp+8]  = host VMCB
    push rdi                    ; [rsp]    = guest VMCB

    ; Save the host's FS/GS/TR/LDTR/KernelGSBase, then load the guest's.
    mov rax, rsi
    vmsave
    mov rax, rdi
    vmload

    ; Load guest GPRs.  RSI and RDI come last because the pointer uses them.
    mov rax, [rsp + 16]
    mov rbx, [rax + 8]
    mov rcx, [rax + 16]
    mov rdx, [rax + 24]
    mov rbp, [rax + 56]
    mov r8,  [rax + 64]
    mov r9,  [rax + 72]
    mov r10, [rax + 80]
    mov r11, [rax + 88]
    mov r12, [rax + 96]
    mov r13, [rax + 104]
    mov r14, [rax + 112]
    mov r15, [rax + 120]
    mov rsi, [rax + 32]
    mov rdi, [rax + 40]
    mov rax, [rsp]              ; VMRUN takes the VMCB address in RAX

    vmrun

    ; #VMEXIT.  Host RSP is back at the pushes above.  Store the guest GPRs.
    mov rax, [rsp + 16]
    mov [rax + 8], rbx
    mov [rax + 16], rcx
    mov [rax + 24], rdx
    mov [rax + 32], rsi
    mov [rax + 40], rdi
    mov [rax + 56], rbp
    mov [rax + 64], r8
    mov [rax + 72], r9
    mov [rax + 80], r10
    mov [rax + 88], r11
    mov [rax + 96], r12
    mov [rax + 104], r13
    mov [rax + 112], r14
    mov [rax + 120], r15

    ; Save the guest's FS/GS/TR/LDTR, then restore the host's.
    mov rax, [rsp]
    vmsave
    mov rax, [rsp + 8]
    vmload
    stgi

    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
