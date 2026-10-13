; No libc or multilib packages required: Linux i386 ABI smoke test.
bits 32
global _start
section .text
_start:
    cld
    cmp dword [esp], 1
    jb fail
    mov esi, [esp + 4]
    test esi, esi
    jz fail
    mov byte [stage], '2'
    ; Walk argv and envp using 4-byte pointers, then inspect auxiliary vector.
    mov ecx, [esp]
    lea esi, [esp + ecx * 4 + 8]
.env:
    lodsd
    test eax, eax
    jnz .env
    xor edi, edi
.aux:
    lodsd
    mov edx, eax
    lodsd
    test edx, edx
    jz .aux_done
    cmp edx, 4                  ; AT_PHENT
    jne .platform
    cmp eax, 32
    jne fail
    or edi, 1
.platform:
    cmp edx, 15                 ; AT_PLATFORM
    jne .aux
    cmp dword [eax], 'i686'
    jne fail
    or edi, 2
    jmp .aux
.aux_done:
    cmp edi, 3
    jne fail
    mov byte [stage], '3'
    mov edi, zero_area
    xor eax, eax
    mov ecx, 2048
    repe scasd
    jne fail
    mov byte [stage], '4'
    mov eax, 20                 ; getpid
    int 0x80
    test eax, eax
    jle fail
    mov byte [stage], '5'
    mov eax, 192                ; mmap2(NULL, 4096, RW, PRIVATE|ANON, -1, 0)
    xor ebx, ebx
    mov ecx, 4096
    mov edx, 3
    mov esi, 0x22
    mov edi, -1
    xor ebp, ebp
    int 0x80
    cmp eax, 0xfffff001
    jae fail
    cmp dword [eax], 0
    jne fail
    mov dword [eax], 0x12345678
    mov byte [stage], '6'
    mov ebx, eax
    mov ecx, 4096
    mov eax, 91                 ; munmap
    int 0x80
    test eax, eax
    jnz fail
    mov byte [stage], '7'
    mov ecx, 0x12345678
    mov esi, 0xabcdef12
    mov edi, 0x76543210
    mov ebp, 0x13572468
    mov eax, 2                  ; fork must preserve these registers in child
    int 0x80
    cmp ecx, 0x12345678
    jne fail
    cmp esi, 0xabcdef12
    jne fail
    cmp edi, 0x76543210
    jne fail
    cmp ebp, 0x13572468
    jne fail
    test eax, eax
    js fail
    jz .child
    mov byte [stage], '8'
    mov ebx, eax
    mov ecx, status
    xor edx, edx
    mov eax, 7                  ; waitpid(child, &status, 0)
    int 0x80
    test eax, eax
    jle fail
    cmp dword [status], 0x2300
    jne fail
    mov ecx, pass_text
    mov edx, pass_len
    xor ebp, ebp
    jmp print_exit
.child:
    mov eax, 1
    mov ebx, 0x23
    int 0x80
    ud2
fail:
    mov al, [stage]
    mov [fail_stage], al
    mov ecx, fail_text
    mov edx, fail_len
    mov ebp, 1
print_exit:
    mov eax, 4
    mov ebx, 1
    int 0x80
    mov eax, 1
    mov ebx, ebp
    int 0x80
    ud2
section .rodata
pass_text: db '[PASS] ELF32: stack, auxv, BSS, int80, mmap2, fork registers and waitpid',10
pass_len equ $ - pass_text
section .data
stage: db '1'
fail_text: db '[FAIL] ELF32 smoke test at stage '
fail_stage: db '0',10
fail_len equ $ - fail_text
section .bss
align 4096
zero_area: resb 8192
status: resd 1
section .note.GNU-stack noalloc noexec nowrite progbits
