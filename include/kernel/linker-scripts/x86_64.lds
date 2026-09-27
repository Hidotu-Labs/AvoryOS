OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS {
    requests PT_LOAD;
    text     PT_LOAD;
    rodata   PT_LOAD;
    data     PT_LOAD;
}

SECTIONS {
    . = 0xffffffff80000000;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :requests

    . = ALIGN(0x1000);

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __start___ex_table = .;
        KEEP(*(__ex_table))
        __stop___ex_table = .;
    } :rodata

    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
