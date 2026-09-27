global gdt_flush

section .text
gdt_flush:
    ; The pointer to the GDT is passed in RDI (System V AMD64 ABI)
    lgdt [rdi]

    ; Load DS, ES, FS, GS, SS with Kernel Data Segment (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far return to reload CS with Kernel Code Segment (0x08)
    ; Push the segment selector (0x08) and the return address, then lretq
    pop rdi          
    mov rax, 0x08    
    push rax         
    push rdi         
    o64 retf         
