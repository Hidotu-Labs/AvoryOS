; SysV x86-64 S4 (hibernate) entry wrapper.
; Saves callee-saved registers and RFLAGS on the stack, snapshots memory and
; writes the hibernation image, then enters ACPI S4 (or S5 fallback).
; Phase 4 resume restores CR3, RSP, and jumps to .resume.

bits 64

global power_s4_enter_arch
global hibernate_reloc_trampoline_start
global hibernate_reloc_trampoline_end
global hibernate_switch_ctx_stub
extern power_s4_snapshot_and_sleep
extern power_s4_resume
extern fpu_on_context_switch

section .text
power_s4_enter_arch:
    pushfq
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    call fpu_on_context_switch

    mov rdi, rsp
    lea rsi, [rel .resume]
    call power_s4_snapshot_and_sleep
    jmp .out

.resume:
    call power_s4_resume

.out:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    ret

; Position-independent S4 page relocation trampoline.
; Copied to a disjoint, identity-mapped executable page before execution.
; SysV ABI:
;   rdi = uint64_t first_ops_phys (linked list of 4 KiB hibernate_ops_page)
;   rsi = uint64_t pml4_tmp_phys
;   rdx = uint64_t hhdm
;   rcx = uint64_t saved_cr3
;   r8  = uint64_t saved_rsp
;   r9  = uint64_t saved_rip
align 16
hibernate_reloc_trampoline_start:
    cli
    mov r13, [rsp + 8]
    mov cr3, rsi
    mov rbx, rdi
    mov r10, rcx
    mov r11, r8
    mov r12, r9
    cld

.page_loop:
    test rbx, rbx
    jz .switch_ctx
    add rbx, rdx            ; rbx = virt of hibernate_ops_page
    mov ebp, [rbx + 4080]   ; count (entries in this page)
    mov r14, [rbx + 4088]   ; next_phys
    test ebp, ebp
    jz .next_page
    mov r15, rbx            ; cursor = &entries[0]
.copy_loop:
    mov rsi, [r15]
    add rsi, rdx
    mov rdi, [r15 + 8]
    add rdi, rdx
    mov rcx, 512
    rep movsq
    add r15, 16
    dec ebp
    jnz .copy_loop
.next_page:
    mov rbx, r14
    jmp .page_loop

.switch_ctx:
    ; Jump to hibernate_switch_ctx_stub in kernel .text (mapped at the same
    ; virtual address in both the temporary CR3 and the restored CR3).
    jmp r13
hibernate_reloc_trampoline_end:

; Executes in kernel .text so the instruction stream stays mapped across
; `mov cr3, r10` into the hibernated kernel's page tables.
hibernate_switch_ctx_stub:
    mov cr3, r10
    mov rsp, r11
    jmp r12
