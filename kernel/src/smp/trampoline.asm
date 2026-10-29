; AP startup trampoline, copied to physical 0x8000 and entered by SIPI.
; Keep the whole image within one page: SIPI vectors name 4 KiB pages.

%define TRAMPOLINE_PHYS       0x8000
%define TRAMPOLINE_MAX_APS    63
%define AP_RECORD_SIZE        24

global trampoline_start
global trampoline_end
global trampoline_data_cr3
global trampoline_data_rip
global trampoline_data_ap_count
global trampoline_ap_records
global trampoline_ap_records_end

section .data
align 4096

trampoline_start:
bits 16
    cli
    cld

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; INIT leaves APs in real mode. The temporary GDT and every reference
    ; below use the fixed SIPI page address because this image is relocated.
    lgdt [gdt32_ptr - trampoline_start + TRAMPOLINE_PHYS]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:(pm32_entry - trampoline_start + TRAMPOLINE_PHYS)

bits 32
pm32_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Preserve the APIC ID across the mode switch. This kernel currently
    ; enumerates 8-bit ACPI APIC IDs, matching CPUID leaf 1's legacy APIC ID.
    mov eax, 1
    cpuid
    mov edi, ebx
    shr edi, 24

    mov eax, cr4
    or eax, 1 << 5                 ; CR4.PAE
    mov cr4, eax

    mov eax, [trampoline_data_cr3 - trampoline_start + TRAMPOLINE_PHYS]
    mov cr3, eax

    mov ecx, 0xC0000080            ; IA32_EFER
    rdmsr
    or eax, 0x0900                 ; EFER.LME | EFER.NXE
    wrmsr

    mov eax, cr0
    or eax, 0x80010000             ; CR0.PG | CR0.WP
    and eax, 0x9FFFFFFF            ; clear CR0.CD | CR0.NW
    mov cr0, eax
    jmp 0x18:(lm64_entry - trampoline_start + TRAMPOLINE_PHYS)

bits 64
lm64_entry:
    ; Find this AP's immutable handoff record. This lets the BSP start APs in
    ; one batch without sharing a mutable stack/CPU pointer between them.
    mov ecx, [abs TRAMPOLINE_PHYS + trampoline_data_ap_count - trampoline_start]
    mov rsi, TRAMPOLINE_PHYS + trampoline_ap_records - trampoline_start
.find_ap:
    test ecx, ecx
    jz .park
    cmp dword [rsi], edi
    je .found
    add rsi, AP_RECORD_SIZE
    dec ecx
    jmp .find_ap

.found:
    mov rsp, [rsi + 8]
    mov rdi, [rsi + 16]            ; SysV first argument: struct cpu_info *
    sub rsp, 8                     ; function-entry ABI alignment (RSP % 16 = 8)
    mov qword [rsp], 0             ; ap_main is not expected to return
    mov rax, [abs TRAMPOLINE_PHYS + trampoline_data_rip - trampoline_start]
    jmp rax

.park:
    cli
.halt:
    hlt
    jmp .halt

align 8
trampoline_data_cr3:       dq 0
trampoline_data_rip:       dq 0
trampoline_data_ap_count:  dd 0
                           dd 0

; Each record is { uint32_t apic_id, uint32_t reserved,
;                  uint64_t stack_top, uint64_t cpu_info }.
align 8
trampoline_ap_records:
%rep TRAMPOLINE_MAX_APS
    dd 0
    dd 0
    dq 0
    dq 0
%endrep
trampoline_ap_records_end:

align 8
gdt32:
    dq 0
    dq 0x00cf9a000000ffff          ; 0x08 32-bit code
    dq 0x00cf92000000ffff          ; 0x10 data
    dq 0x00af9a000000ffff          ; 0x18 64-bit code
gdt32_ptr:
    dw $ - gdt32 - 1
    dd gdt32 - trampoline_start + TRAMPOLINE_PHYS

trampoline_end:

%if (trampoline_end - trampoline_start) > 4096
    %error "AP trampoline must fit within its SIPI page"
%endif
