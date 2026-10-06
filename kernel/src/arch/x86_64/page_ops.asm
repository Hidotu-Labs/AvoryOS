; page_ops.asm — Hardware-accelerated non-temporal page zeroing and copying
;
; Implements 512-bit AVX-512 non-temporal operations for AMD Zen 4 (Ryzen 5 7600)
; and compatible CPUs, with AVX2 (256-bit) and ERMS (rep stosq/movsq) fallbacks.
; Non-temporal stores (vmovntdq) write directly via write-combining (WC) buffers,
; bypassing L1/L2/L3 caches to prevent cache pollution and eliminate read-for-ownership (RFO).

global page_zero_nt
global page_copy_nt
global page_zero_erms
global page_copy_erms
global page_zero_avx512
global page_copy_avx512
global page_zero_avx2
global page_copy_avx2

section .text

; ==============================================================================
; Integer Non-Temporal Page Operations (64-bit / MOVNTI)
; Safe for interrupt, exception, and page-fault handlers (does NOT clobber FPU/SIMD)
; Writes via write-combining (WC) buffers, bypassing L1/L2/L3 caches.
; ==============================================================================

; void page_zero_nt(void *page)  ; RDI = page (4096 bytes, 64-byte aligned)
align 64
page_zero_nt:
    xor eax, eax
    mov ecx, 32             ; 32 iterations * 128 bytes = 4096 bytes

.loop_zero_nt:
    movnti [rdi], rax
    movnti [rdi + 8], rax
    movnti [rdi + 16], rax
    movnti [rdi + 24], rax
    movnti [rdi + 32], rax
    movnti [rdi + 40], rax
    movnti [rdi + 48], rax
    movnti [rdi + 56], rax
    movnti [rdi + 64], rax
    movnti [rdi + 72], rax
    movnti [rdi + 80], rax
    movnti [rdi + 88], rax
    movnti [rdi + 96], rax
    movnti [rdi + 104], rax
    movnti [rdi + 112], rax
    movnti [rdi + 120], rax
    add rdi, 128
    dec ecx
    jnz .loop_zero_nt

    sfence                  ; Drain write-combining buffers
    ret

; void page_copy_nt(void *dst, const void *src)  ; RDI = dst, RSI = src
align 64
page_copy_nt:
    mov ecx, 32             ; 32 iterations * 128 bytes = 4096 bytes

.loop_copy_nt:
    prefetchnta [rsi + 256]
    prefetchnta [rsi + 320]

    mov rax, [rsi]
    mov r8,  [rsi + 8]
    mov r9,  [rsi + 16]
    mov r10, [rsi + 24]
    mov r11, [rsi + 32]
    mov rdx, [rsi + 40]

    movnti [rdi], rax
    movnti [rdi + 8], r8
    movnti [rdi + 16], r9
    movnti [rdi + 24], r10
    movnti [rdi + 32], r11
    movnti [rdi + 40], rdx

    mov rax, [rsi + 48]
    mov r8,  [rsi + 56]
    mov r9,  [rsi + 64]
    mov r10, [rsi + 72]
    mov r11, [rsi + 80]
    mov rdx, [rsi + 88]

    movnti [rdi + 48], rax
    movnti [rdi + 56], r8
    movnti [rdi + 64], r9
    movnti [rdi + 72], r10
    movnti [rdi + 80], r11
    movnti [rdi + 88], rdx

    mov rax, [rsi + 96]
    mov r8,  [rsi + 104]
    mov r9,  [rsi + 112]
    mov r10, [rsi + 120]

    movnti [rdi + 96], rax
    movnti [rdi + 104], r8
    movnti [rdi + 112], r9
    movnti [rdi + 120], r10

    add rsi, 128
    add rdi, 128
    dec ecx
    jnz .loop_copy_nt

    sfence                  ; Drain write-combining buffers
    ret

; ==============================================================================
; AVX-512 Non-Temporal Page Operations (512-bit / 64 bytes per vector)
; WARNING: Requires kernel_fpu_begin() if called in kernel context!
; ==============================================================================

; void page_zero_avx512(void *page)  ; RDI = page (4096 bytes, 64-byte aligned)
align 64
page_zero_avx512:
    vpxord zmm0, zmm0, zmm0
    mov ecx, 16             ; 16 iterations * 256 bytes = 4096 bytes

.loop:
    vmovntdq [rdi], zmm0
    vmovntdq [rdi + 64], zmm0
    vmovntdq [rdi + 128], zmm0
    vmovntdq [rdi + 192], zmm0
    add rdi, 256
    dec ecx
    jnz .loop

    sfence                  ; Drain write-combining buffers
    vzeroupper
    ret

; void page_copy_avx512(void *dst, const void *src)  ; RDI = dst, RSI = src
align 64
page_copy_avx512:
    mov ecx, 16             ; 16 iterations * 256 bytes = 4096 bytes

.loop:
    prefetchnta [rsi + 512] ; Prefetch future cachelines without cache pollution
    prefetchnta [rsi + 576]
    prefetchnta [rsi + 640]
    prefetchnta [rsi + 704]

    vmovdqa64 zmm0, [rsi]
    vmovdqa64 zmm1, [rsi + 64]
    vmovdqa64 zmm2, [rsi + 128]
    vmovdqa64 zmm3, [rsi + 192]

    vmovntdq [rdi], zmm0
    vmovntdq [rdi + 64], zmm1
    vmovntdq [rdi + 128], zmm2
    vmovntdq [rdi + 192], zmm3

    add rsi, 256
    add rdi, 256
    dec ecx
    jnz .loop

    sfence                  ; Drain write-combining buffers
    vzeroupper
    ret

; ==============================================================================
; AVX2 Non-Temporal Page Operations (256-bit / 32 bytes per vector)
; ==============================================================================

; void page_zero_avx2(void *page)  ; RDI = page (4096 bytes)
align 32
page_zero_avx2:
    vpxor ymm0, ymm0, ymm0
    mov ecx, 32             ; 32 iterations * 128 bytes = 4096 bytes

.loop:
    vmovntdq [rdi], ymm0
    vmovntdq [rdi + 32], ymm0
    vmovntdq [rdi + 64], ymm0
    vmovntdq [rdi + 96], ymm0
    add rdi, 128
    dec ecx
    jnz .loop

    sfence
    vzeroupper
    ret

; void page_copy_avx2(void *dst, const void *src)  ; RDI = dst, RSI = src
align 32
page_copy_avx2:
    mov ecx, 32             ; 32 iterations * 128 bytes = 4096 bytes

.loop:
    prefetchnta [rsi + 256]
    prefetchnta [rsi + 320]

    vmovdqa ymm0, [rsi]
    vmovdqa ymm1, [rsi + 32]
    vmovdqa ymm2, [rsi + 64]
    vmovdqa ymm3, [rsi + 96]

    vmovntdq [rdi], ymm0
    vmovntdq [rdi + 32], ymm1
    vmovntdq [rdi + 64], ymm2
    vmovntdq [rdi + 96], ymm3

    add rsi, 128
    add rdi, 128
    dec ecx
    jnz .loop

    sfence
    vzeroupper
    ret

; ==============================================================================
; ERMS / Legacy Fallback Page Operations (rep stosq / rep movsq)
; ==============================================================================

; void page_zero_erms(void *page)  ; RDI = page
align 16
page_zero_erms:
    xor eax, eax
    mov ecx, 512            ; 512 * 8 bytes = 4096 bytes
    rep stosq
    ret

; void page_copy_erms(void *dst, const void *src)  ; RDI = dst, RSI = src
align 16
page_copy_erms:
    mov ecx, 512            ; 512 * 8 bytes = 4096 bytes
    rep movsq
    ret
