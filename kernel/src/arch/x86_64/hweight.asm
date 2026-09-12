; hweight.asm — software popcount helpers with the x86 kernel's special ABI.
;
; Imported <asm/arch_hweight.h> expands hweight32()/hweight64() to a raw
;   call __sw_hweight32 / __sw_hweight64
; inside an inline asm with an empty clobber list, because upstream x86
; provides these in arch/x86/lib/hweight.S and guarantees they preserve every
; register except the return register.  A normal C implementation only
; preserves callee-saved registers, so the caller's live registers are
; silently corrupted (observed with drm_property_create_bitmask() losing its
; name/flags arguments across hweight64(supported_bits)).
;
; These are software implementations (no POPCNT requirement) and preserve
; RDI/RSI/RDX/RCX/R8-R11 explicitly.  Return value: EAX (32-bit) / RAX (64-bit).

global __sw_hweight32
global __sw_hweight64

section .text

; unsigned int __sw_hweight32(unsigned int w)   ; w = EDI -> EAX
__sw_hweight32:
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    push r10
    push r11

    mov eax, edi
    mov ecx, eax
    shr ecx, 1
    and ecx, 0x55555555
    sub eax, ecx          ; x -= (x >> 1) & 0x55555555
    mov ecx, eax
    shr ecx, 2
    and eax, 0x33333333
    and ecx, 0x33333333
    add eax, ecx          ; x = (x & 0x33333333) + ((x >> 2) & 0x33333333)
    mov ecx, eax
    shr ecx, 4
    add eax, ecx
    and eax, 0x0f0f0f0f   ; x = (x + (x >> 4)) & 0x0f0f0f0f
    imul eax, eax, 0x01010101
    shr eax, 24           ; (x * 0x01010101) >> 24

    pop r11
    pop r10
    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    ret

; unsigned long __sw_hweight64(unsigned long long w) ; w = RDI -> RAX
__sw_hweight64:
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    push r10
    push r11

    mov rax, rdi
    mov rcx, rax
    shr rcx, 1
    mov rdx, 0x5555555555555555
    and rcx, rdx
    sub rax, rcx
    mov rcx, rax
    shr rcx, 2
    mov rdx, 0x3333333333333333
    and rax, rdx
    and rcx, rdx
    add rax, rcx
    mov rcx, rax
    shr rcx, 4
    add rax, rcx
    mov rdx, 0x0f0f0f0f0f0f0f0f
    and rax, rdx
    mov rdx, 0x0101010101010101
    imul rax, rdx
    shr rax, 56

    pop r11
    pop r10
    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    ret
