; uaccess.asm — Exception-table protected high-performance user memory access routines

global copy_from_user
global copy_to_user
global clear_user
global strncpy_from_user
global strnlen_user

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

    mov rcx, rdx
    cld

cfu_copy_insn:
    rep movsb

cfu_success:
    xor eax, eax
    USER_ACCESS_END
    ret

cfu_error_all:
    mov rax, rdx
    USER_ACCESS_END
    ret

cfu_fixup_landing:
    push rcx
    xor eax, eax
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

    mov rcx, rdx
    cld

ctu_copy_insn:
    rep movsb

ctu_success:
    xor eax, eax
    USER_ACCESS_END
    ret

ctu_error_all:
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

    mov rcx, rsi
    xor eax, eax
    cld

cu_clear_insn:
    rep stosb

cu_success:
    xor eax, eax
    USER_ACCESS_END
    ret

cu_error_all:
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
    dq cfu_copy_insn, cfu_fixup_landing
    dq ctu_copy_insn, ctu_fixup_landing
    dq cu_clear_insn, cu_fixup_landing
    dq sn_align_fetch_insn, sn_fixup_landing
    dq sn_qword_fetch_insn, sn_fixup_landing
    dq sn_sub_fetch_insn,   sn_fixup_landing
    dq sn_tail_fetch_insn,  sn_fixup_landing
    dq sl_align_fetch_insn, sl_fixup_landing
    dq sl_qword_fetch_insn, sl_fixup_landing
    dq sl_tail_fetch_insn,  sl_fixup_landing
