global gdt_flush

section .text
gdt_flush:
    ; The pointer to the GDT is passed in RDI (System V AMD64 ABI)
    lgdt [rdi]

    ; Preserve IA32_GS_BASE across segment selector reload (`mov gs, ax` zeroes it).
    mov ecx, 0xC0000101
    rdmsr
    mov r8d, eax
    mov r9d, edx

    ; Load DS, ES, FS, GS, SS with Kernel Data Segment (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov ecx, 0xC0000101
    mov eax, r8d
    mov edx, r9d
    wrmsr

    ; Far return to reload CS with Kernel Code Segment (0x08)
    ; Push the segment selector (0x08) and the return address, then lretq
    pop rdi          
    mov rax, 0x08    
    push rax         
    push rdi         
    o64 retf         
