[bits 64]

global cpu_switch_stack
global cpu_jump_to_stack

section .text

; void cpu_switch_stack(uint64_t new_rsp)
; Safely switches the stack and returns into the caller.
cpu_switch_stack:
    ; Preserve the caller's interrupt state. AP startup calls this with
    ; interrupts disabled and does not have a valid IDT yet, so an
    ; unconditional STI here can make a real AP triple-fault.
    pushfq
    pop rdx
    cli
    ; The current stack has the return address at [rsp].
    pop rsi
    mov rsp, rdi            ; Switch to the new stack top
    push rsi                ; Push return address onto the new stack
    push rdx
    popfq                    ; Restore IF (and the other caller flags)
    ret

; void cpu_jump_to_stack(uint64_t new_rsp, void (*target)(void))
; Discards current stack and jumps to a new function on a new stack.
cpu_jump_to_stack:
    cli
    mov rsp, rdi            ; Switch stacks
    sti
    jmp rsi                 ; Jump to target function
