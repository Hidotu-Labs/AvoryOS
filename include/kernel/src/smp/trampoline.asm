global trampoline_start
global trampoline_end
global trampoline_data_cr3
global trampoline_data_rip
global trampoline_data_stack

section .data
align 4096

trampoline_start:
bits 16
    cli
    cld

    ; Reset segment registers to explicitly 0
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load temporary GDT from physical address 0x8000 + offset
    lgdt [gdt32_ptr - trampoline_start + 0x8000]

    ; Enable protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Jump to 32-bit code
    jmp dword 0x08:(pm32_entry - trampoline_start + 0x8000)

bits 32
pm32_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Load CR3
    mov eax, [trampoline_data_cr3 - trampoline_start + 0x8000]
    mov cr3, eax

    ; Enable Long Mode (LME=bit 8) and NXE (bit 11) in EFER
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x0900
    wrmsr

    ; Enable Paging (PG=bit 31), WP (bit 16), and clear CD/NW (bits 30/29)
    mov eax, cr0
    or eax, 0x80010000
    and eax, 0x9FFFFFFF
    mov cr0, eax

    ; Jump to 64-bit mode
    jmp 0x18:(lm64_entry - trampoline_start + 0x8000)

bits 64
lm64_entry:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Setup stack
    mov rsp, [trampoline_data_stack - trampoline_start + 0x8000]
    mov rbp, rsp

    ; Jump to C code (ap_main)
    mov rax, [trampoline_data_rip - trampoline_start + 0x8000]
    jmp rax

align 8
trampoline_data_cr3:   dq 0
trampoline_data_rip:   dq 0
trampoline_data_stack: dq 0

align 8
gdt32:
    dq 0
    dq 0x00cf9a000000ffff ; 0x08 code32
    dq 0x00cf92000000ffff ; 0x10 data32
    dq 0x00af9a000000ffff ; 0x18 code64
gdt32_ptr:
    dw $ - gdt32 - 1
    dd gdt32 - trampoline_start + 0x8000

trampoline_end:
