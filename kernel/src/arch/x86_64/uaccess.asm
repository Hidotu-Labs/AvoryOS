; uaccess.asm — Exception-table protected high-performance user memory access routines

global copy_from_user
global copy_to_user
global clear_user
global strncpy_from_user
global strnlen_user
global cmpxchg_user_u32

extern cpu_has_smap_flag

section .text

; Open a user-access window for one routine.  The caller's RFLAGS are saved on
; the stack and restored by USER_ACCESS_END, so an outer coarse window (AC
; already set by the syscall entry path) is preserved rather than clobbered.
; stac is #UD on CPUs without SMAP, hence the runtime gate.
%macro USER_ACCESS_BEGIN 0
    pushfq
    cmp byte [rel cpu_has_smap_flag], 0
    je %%smap_done
    stac
%%smap_done:
%endmacro

%macro USER_ACCESS_END 0
    popfq
%endmacro

; Atomic robust-futex update, including an SMAP window and fault fixup.
; rdi=user word, rsi=kernel expected word, edx=desired word.
cmpxchg_user_u32:
    USER_ACCESS_BEGIN
    mov rax, rdi
    add rax, 4
    jc cxu_fault
    mov r8, 0x800000000000
    cmp rax, r8
    ja cxu_fault
    mov eax, [rsi]
cxu_atomic_insn:
    lock cmpxchg [rdi], edx
    sete cl
    mov [rsi], eax
    movzx eax, cl
    USER_ACCESS_END
    ret
cxu_fault:
    mov eax, -14
    USER_ACCESS_END
    ret

; -----------------------------------------------------------------------------
; unsigned long copy_from_user(void *to, const void *from, unsigned long n)
; rdi = to (kernel dst), rsi = from (user src), rdx = n (bytes)
; -----------------------------------------------------------------------------
copy_from_user:
    USER_ACCESS_BEGIN
    test rdx, rdx
    jz cfu_success

    ; Bounds check: from <= 0x7FFFFFFFFFFF and from + n <= 0x800000000000
    mov rax, rsi
    add rax, rdx
    jc cfu_error_all
    mov r8, 0x800000000000
    cmp rax, r8
    ja cfu_error_all

    ; Fast path dispatch based on size
    cmp rdx, 8
    jb cfu_small
    je cfu_exact_8
    cmp rdx, 16
    je cfu_exact_16
    cmp rdx, 64
    jae cfu_bulk

    ; Arbitrary sizes 8 < n < 64: copy 8 bytes at a time
cfu_qwords:
cfu_fetch_qword:
    mov rax, qword [rsi]
    mov qword [rdi], rax
    add rsi, 8
    add rdi, 8
    sub rdx, 8
    cmp rdx, 8
    jae cfu_qwords

cfu_tail:
    test rdx, rdx
    jz cfu_success

    test dl, 4
    jz cfu_tail_no_4
cfu_fetch_tail_4:
    mov eax, dword [rsi]
    mov dword [rdi], eax
    add rsi, 4
    add rdi, 4
    sub rdx, 4
cfu_tail_no_4:
    test dl, 2
    jz cfu_tail_no_2
cfu_fetch_tail_2:
    mov ax, word [rsi]
    mov word [rdi], ax
    add rsi, 2
    add rdi, 2
    sub rdx, 2
cfu_tail_no_2:
    test dl, 1
    jz cfu_success
cfu_fetch_tail_1:
    mov al, byte [rsi]
    mov byte [rdi], al
cfu_success:
    xor eax, eax
    USER_ACCESS_END
    ret

cfu_small:
    cmp rdx, 4
    je cfu_exact_4
    cmp rdx, 1
    je cfu_exact_1
    cmp rdx, 2
    je cfu_exact_2
    jmp cfu_tail

cfu_exact_1:
cfu_fetch_1:
    mov al, byte [rsi]
    mov byte [rdi], al
    xor eax, eax
    USER_ACCESS_END
    ret

cfu_exact_2:
cfu_fetch_2:
    mov ax, word [rsi]
    mov word [rdi], ax
    xor eax, eax
    USER_ACCESS_END
    ret

cfu_exact_4:
cfu_fetch_4:
    mov eax, dword [rsi]
    mov dword [rdi], eax
    xor eax, eax
    USER_ACCESS_END
    ret

cfu_exact_8:
cfu_fetch_8:
    mov rax, qword [rsi]
    mov qword [rdi], rax
    xor eax, eax
    USER_ACCESS_END
    ret

cfu_exact_16:
cfu_fetch_16_0:
    mov rax, qword [rsi]
    mov qword [rdi], rax
cfu_fetch_16_1:
    mov r8, qword [rsi + 8]
    mov qword [rdi + 8], r8
    xor eax, eax
    USER_ACCESS_END
    ret

cfu_bulk:
    mov rcx, rdx
    cld
cfu_copy_insn:
    rep movsb
    xor eax, eax
    USER_ACCESS_END
    ret

cfu_error_all:
    mov rax, rdx
    USER_ACCESS_END
    ret

; Fixup handlers for copy_from_user:
cfu_fixup_1:
    mov byte [rdi], 0
    mov eax, 1
    USER_ACCESS_END
    ret

cfu_fixup_2:
    mov word [rdi], 0
    mov eax, 2
    USER_ACCESS_END
    ret

cfu_fixup_4:
    mov dword [rdi], 0
    mov eax, 4
    USER_ACCESS_END
    ret

cfu_fixup_8:
    mov qword [rdi], 0
    mov eax, 8
    USER_ACCESS_END
    ret

cfu_fixup_16_0:
    mov qword [rdi], 0
    mov qword [rdi + 8], 0
    mov eax, 16
    USER_ACCESS_END
    ret

cfu_fixup_16_1:
    mov qword [rdi + 8], 0
    mov eax, 8
    USER_ACCESS_END
    ret

cfu_fixup_rdx:
    mov rcx, rdx
    xor eax, eax
    cld
    rep stosb
    mov rax, rdx
    USER_ACCESS_END
    ret

cfu_fixup_landing:
    push rcx
    xor eax, eax
    cld
    rep stosb
    pop rax
    USER_ACCESS_END
    ret

; -----------------------------------------------------------------------------
; unsigned long copy_to_user(void *to, const void *from, unsigned long n)
; rdi = to (user dst), rsi = from (kernel src), rdx = n (bytes)
; -----------------------------------------------------------------------------
copy_to_user:
    USER_ACCESS_BEGIN
    test rdx, rdx
    jz ctu_success

    ; Bounds check: to <= 0x7FFFFFFFFFFF and to + n <= 0x800000000000
    mov rax, rdi
    add rax, rdx
    jc ctu_error_all
    mov r8, 0x800000000000
    cmp rax, r8
    ja ctu_error_all

    ; Fast path dispatch based on size
    cmp rdx, 8
    jb ctu_small
    je ctu_exact_8
    cmp rdx, 16
    je ctu_exact_16
    cmp rdx, 64
    jae ctu_bulk

    ; Arbitrary sizes 8 < n < 64: copy 8 bytes at a time
ctu_qwords:
    mov rax, qword [rsi]
ctu_store_qword:
    mov qword [rdi], rax
    add rsi, 8
    add rdi, 8
    sub rdx, 8
    cmp rdx, 8
    jae ctu_qwords

ctu_tail:
    test rdx, rdx
    jz ctu_success

    test dl, 4
    jz ctu_tail_no_4
    mov eax, dword [rsi]
ctu_store_tail_4:
    mov dword [rdi], eax
    add rsi, 4
    add rdi, 4
    sub rdx, 4
ctu_tail_no_4:
    test dl, 2
    jz ctu_tail_no_2
    mov ax, word [rsi]
ctu_store_tail_2:
    mov word [rdi], ax
    add rsi, 2
    add rdi, 2
    sub rdx, 2
ctu_tail_no_2:
    test dl, 1
    jz ctu_success
    mov al, byte [rsi]
ctu_store_tail_1:
    mov byte [rdi], al
ctu_success:
    xor eax, eax
    USER_ACCESS_END
    ret

ctu_small:
    cmp rdx, 4
    je ctu_exact_4
    cmp rdx, 1
    je ctu_exact_1
    cmp rdx, 2
    je ctu_exact_2
    jmp ctu_tail

ctu_exact_1:
    mov al, byte [rsi]
ctu_store_1:
    mov byte [rdi], al
    xor eax, eax
    USER_ACCESS_END
    ret

ctu_exact_2:
    mov ax, word [rsi]
ctu_store_2:
    mov word [rdi], ax
    xor eax, eax
    USER_ACCESS_END
    ret

ctu_exact_4:
    mov eax, dword [rsi]
ctu_store_4:
    mov dword [rdi], eax
    xor eax, eax
    USER_ACCESS_END
    ret

ctu_exact_8:
    mov rax, qword [rsi]
ctu_store_8:
    mov qword [rdi], rax
    xor eax, eax
    USER_ACCESS_END
    ret

ctu_exact_16:
    mov rax, qword [rsi]
    mov r8, qword [rsi + 8]
ctu_store_16_0:
    mov qword [rdi], rax
ctu_store_16_1:
    mov qword [rdi + 8], r8
    xor eax, eax
    USER_ACCESS_END
    ret

ctu_bulk:
    mov rcx, rdx
    cld
ctu_copy_insn:
    rep movsb
    xor eax, eax
    USER_ACCESS_END
    ret

ctu_error_all:
    mov rax, rdx
    USER_ACCESS_END
    ret

; Fixup handlers for copy_to_user:
ctu_fixup_1:
    mov eax, 1
    USER_ACCESS_END
    ret

ctu_fixup_2:
    mov eax, 2
    USER_ACCESS_END
    ret

ctu_fixup_4:
    mov eax, 4
    USER_ACCESS_END
    ret

ctu_fixup_8:
    mov eax, 8
    USER_ACCESS_END
    ret

ctu_fixup_16_0:
    mov eax, 16
    USER_ACCESS_END
    ret

ctu_fixup_16_1:
    mov eax, 8
    USER_ACCESS_END
    ret

ctu_fixup_rdx:
    mov rax, rdx
    USER_ACCESS_END
    ret

ctu_fixup_landing:
    mov rax, rcx
    USER_ACCESS_END
    ret

; -----------------------------------------------------------------------------
; unsigned long clear_user(void *to, unsigned long n)
; rdi = to (user dst), rsi = n (bytes)
; -----------------------------------------------------------------------------
clear_user:
    USER_ACCESS_BEGIN
    test rsi, rsi
    jz cu_success

    mov rax, rdi
    add rax, rsi
    jc cu_error_all
    mov r8, 0x800000000000
    cmp rax, r8
    ja cu_error_all

    cmp rsi, 8
    jb cu_small
    je cu_exact_8
    cmp rsi, 16
    je cu_exact_16
    cmp rsi, 64
    jae cu_bulk

cu_qwords:
cu_store_qword:
    mov qword [rdi], 0
    add rdi, 8
    sub rsi, 8
    cmp rsi, 8
    jae cu_qwords

cu_tail:
    test rsi, rsi
    jz cu_success

    test sil, 4
    jz cu_tail_no_4
cu_store_tail_4:
    mov dword [rdi], 0
    add rdi, 4
    sub rsi, 4
cu_tail_no_4:
    test sil, 2
    jz cu_tail_no_2
cu_store_tail_2:
    mov word [rdi], 0
    add rdi, 2
    sub rsi, 2
cu_tail_no_2:
    test sil, 1
    jz cu_success
cu_store_tail_1:
    mov byte [rdi], 0
cu_success:
    xor eax, eax
    USER_ACCESS_END
    ret

cu_small:
    cmp rsi, 4
    je cu_exact_4
    cmp rsi, 1
    je cu_exact_1
    cmp rsi, 2
    je cu_exact_2
    jmp cu_tail

cu_exact_1:
cu_store_1:
    mov byte [rdi], 0
    xor eax, eax
    USER_ACCESS_END
    ret

cu_exact_2:
cu_store_2:
    mov word [rdi], 0
    xor eax, eax
    USER_ACCESS_END
    ret

cu_exact_4:
cu_store_4:
    mov dword [rdi], 0
    xor eax, eax
    USER_ACCESS_END
    ret

cu_exact_8:
cu_store_8:
    mov qword [rdi], 0
    xor eax, eax
    USER_ACCESS_END
    ret

cu_exact_16:
cu_store_16_0:
    mov qword [rdi], 0
cu_store_16_1:
    mov qword [rdi + 8], 0
    xor eax, eax
    USER_ACCESS_END
    ret

cu_bulk:
    mov rcx, rsi
    xor eax, eax
    cld
cu_clear_insn:
    rep stosb
    xor eax, eax
    USER_ACCESS_END
    ret

cu_error_all:
    mov rax, rsi
    USER_ACCESS_END
    ret

cu_fixup_1:
    mov eax, 1
    USER_ACCESS_END
    ret

cu_fixup_2:
    mov eax, 2
    USER_ACCESS_END
    ret

cu_fixup_4:
    mov eax, 4
    USER_ACCESS_END
    ret

cu_fixup_8:
    mov eax, 8
    USER_ACCESS_END
    ret

cu_fixup_16_0:
    mov eax, 16
    USER_ACCESS_END
    ret

cu_fixup_16_1:
    mov eax, 8
    USER_ACCESS_END
    ret

cu_fixup_rsi:
    mov rax, rsi
    USER_ACCESS_END
    ret

cu_fixup_landing:
    mov rax, rcx
    USER_ACCESS_END
    ret

; -----------------------------------------------------------------------------
; long strncpy_from_user(char *dst, const char *src, long count)
; Fast Word-at-a-time (8-byte QWORD) string copy with zero-byte detection
; rdi = dst (kernel), rsi = src (user), rdx = count
; Returns: number of characters copied (excluding trailing NUL), or -EFAULT (-14)
; -----------------------------------------------------------------------------
strncpy_from_user:
    USER_ACCESS_BEGIN
    test rdx, rdx
    jle sn_zero

    ; Bounds check: src + count <= 0x800000000000
    mov rax, rsi
    add rax, rdx
    jc sn_fault
    mov r8, 0x800000000000
    cmp rax, r8
    ja sn_fault

    xor rax, rax            ; rax = return count

    ; 1. Align source pointer to 8-byte boundary
sn_align_loop:
    test rdx, rdx
    jz sn_done
    test rsi, 7
    jz sn_aligned_start

sn_align_fetch_insn:
    mov cl, byte [rsi]
    mov byte [rdi], cl
    test cl, cl
    jz sn_done
    inc rsi
    inc rdi
    inc rax
    dec rdx
    jmp sn_align_loop

sn_aligned_start:
    mov r9,  0x0101010101010101
    mov r10, 0x8080808080808080

    ; 2. Fast 8-byte QWORD loop
sn_qword_loop:
    cmp rdx, 8
    jb sn_tail_loop

sn_qword_fetch_insn:
    mov r8, [rsi]           ; Load 8 bytes from user space
    mov r11, r8
    sub r11, r9             ; r11 = v - 0x0101010101010101
    not r8                  ; r8 = ~v
    and r11, r8             ; r11 = (v - 0x0101010101010101) & ~v
    not r8                  ; r8 = v
    and r11, r10            ; r11 = zero byte indicator mask
    jnz sn_zero_in_qword

    ; No zero byte in this entire 8-byte word: store all 8 bytes to destination
    mov [rdi], r8
    add rsi, 8
    add rdi, 8
    add rax, 8
    sub rdx, 8
    jmp sn_qword_loop

sn_zero_in_qword:
    bsf r11, r11            ; Bit index of lowest zero byte (7, 15, 23, ...)
    shr r11, 3              ; r11 = byte index of first NUL byte (0..7)
    test r11, r11
    jz sn_store_nul_only

sn_sub_loop:
sn_sub_fetch_insn:
    mov cl, byte [rsi]
    mov byte [rdi], cl
    inc rsi
    inc rdi
    inc rax
    dec r11
    jnz sn_sub_loop

sn_store_nul_only:
    mov byte [rdi], 0
    USER_ACCESS_END
    ret

    ; 3. Remaining bytes (< 8 bytes)
sn_tail_loop:
    test rdx, rdx
    jz sn_done

sn_tail_fetch_insn:
    mov cl, byte [rsi]
    mov byte [rdi], cl
    test cl, cl
    jz sn_done
    inc rsi
    inc rdi
    inc rax
    dec rdx
    jmp sn_tail_loop

sn_done:
    USER_ACCESS_END
    ret

sn_zero:
    xor eax, eax
    USER_ACCESS_END
    ret

sn_fault:
sn_fixup_landing:
    mov rax, -14            ; -EFAULT
    USER_ACCESS_END
    ret

; -----------------------------------------------------------------------------
; long strnlen_user(const char *src, long maxlen)
; Fast Word-at-a-time (8-byte QWORD) string length scanner
; rdi = src (user), rsi = maxlen
; Returns: length INCLUDING the trailing NUL character, or 0 on fault
; -----------------------------------------------------------------------------
strnlen_user:
    USER_ACCESS_BEGIN
    test rsi, rsi
    jle sl_zero

    ; Bounds check: src + maxlen <= 0x800000000000
    mov rax, rdi
    add rax, rsi
    jc sl_fault
    mov r8, 0x800000000000
    cmp rax, r8
    ja sl_fault

    xor rax, rax

    ; 1. Align source pointer to 8-byte boundary
sl_align_loop:
    test rsi, rsi
    jz sl_done_count
    test rdi, 7
    jz sl_aligned_start

sl_align_fetch_insn:
    mov cl, byte [rdi]
    inc rax
    inc rdi
    dec rsi
    test cl, cl
    jz sl_done
    jmp sl_align_loop

sl_aligned_start:
    mov r9,  0x0101010101010101
    mov r10, 0x8080808080808080

    ; 2. Fast 8-byte QWORD loop
sl_qword_loop:
    cmp rsi, 8
    jb sl_tail_loop

sl_qword_fetch_insn:
    mov r8, [rdi]
    mov r11, r8
    sub r11, r9
    not r8
    and r11, r8
    and r11, r10
    jnz sl_zero_in_qword

    add rdi, 8
    add rax, 8
    sub rsi, 8
    jmp sl_qword_loop

sl_zero_in_qword:
    bsf r11, r11
    shr r11, 3              ; Number of non-zero bytes (0..7)
    add rax, r11
    inc rax                 ; +1 for the NUL byte itself
    USER_ACCESS_END
    ret

    ; 3. Tail bytes
sl_tail_loop:
    test rsi, rsi
    jz sl_done_count

sl_tail_fetch_insn:
    mov cl, byte [rdi]
    inc rax
    inc rdi
    dec rsi
    test cl, cl
    jz sl_done
    jmp sl_tail_loop

sl_done_count:
sl_done:
    USER_ACCESS_END
    ret

sl_zero:
sl_fault:
sl_fixup_landing:
    xor eax, eax
    USER_ACCESS_END
    ret

; -----------------------------------------------------------------------------
; Exception Fixup Table
; -----------------------------------------------------------------------------
section __ex_table progbits alloc noexec nowrite
    dq cxu_atomic_insn, cxu_fault
    dq cfu_fetch_1, cfu_fixup_1
    dq cfu_fetch_2, cfu_fixup_2
    dq cfu_fetch_4, cfu_fixup_4
    dq cfu_fetch_8, cfu_fixup_8
    dq cfu_fetch_16_0, cfu_fixup_16_0
    dq cfu_fetch_16_1, cfu_fixup_16_1
    dq cfu_fetch_qword, cfu_fixup_rdx
    dq cfu_fetch_tail_4, cfu_fixup_rdx
    dq cfu_fetch_tail_2, cfu_fixup_rdx
    dq cfu_fetch_tail_1, cfu_fixup_rdx
    dq cfu_copy_insn, cfu_fixup_landing
    dq ctu_store_1, ctu_fixup_1
    dq ctu_store_2, ctu_fixup_2
    dq ctu_store_4, ctu_fixup_4
    dq ctu_store_8, ctu_fixup_8
    dq ctu_store_16_0, ctu_fixup_16_0
    dq ctu_store_16_1, ctu_fixup_16_1
    dq ctu_store_qword, ctu_fixup_rdx
    dq ctu_store_tail_4, ctu_fixup_rdx
    dq ctu_store_tail_2, ctu_fixup_rdx
    dq ctu_store_tail_1, ctu_fixup_rdx
    dq ctu_copy_insn, ctu_fixup_landing
    dq cu_store_1, cu_fixup_1
    dq cu_store_2, cu_fixup_2
    dq cu_store_4, cu_fixup_4
    dq cu_store_8, cu_fixup_8
    dq cu_store_16_0, cu_fixup_16_0
    dq cu_store_16_1, cu_fixup_16_1
    dq cu_store_qword, cu_fixup_rsi
    dq cu_store_tail_4, cu_fixup_rsi
    dq cu_store_tail_2, cu_fixup_rsi
    dq cu_store_tail_1, cu_fixup_rsi
    dq cu_clear_insn, cu_fixup_landing
    dq sn_align_fetch_insn, sn_fixup_landing
    dq sn_qword_fetch_insn, sn_fixup_landing
    dq sn_sub_fetch_insn,   sn_fixup_landing
    dq sn_tail_fetch_insn,  sn_fixup_landing
    dq sl_align_fetch_insn, sl_fixup_landing
    dq sl_qword_fetch_insn, sl_fixup_landing
    dq sl_tail_fetch_insn,  sl_fixup_landing
