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

    /* LinuxKPI initcalls (module_init and friends).  Upstream Linux places
     * these function pointers in .initcall<level>.init sections.  The walker
     * in kernel/src/linuxkpi/init.c iterates __initcall_start..__initcall_end
     * in link order; the per-level symbols mirror what Linux's do_initcalls()
     * expects if we ever import it.  KEEP is mandatory: --gc-sections would
     * otherwise discard the sections and their referenced functions. */
    .initcall : ALIGN(16) {
        __initcall_start = .;
        __initcall0_start = .;
        KEEP(*(.initcall0.init))
        KEEP(*(.initcall0s.init))
        __initcall1_start = .;
        KEEP(*(.initcall1.init))
        KEEP(*(.initcall1s.init))
        __initcall2_start = .;
        KEEP(*(.initcall2.init))
        KEEP(*(.initcall2s.init))
        __initcall3_start = .;
        KEEP(*(.initcall3.init))
        KEEP(*(.initcall3s.init))
        __initcall4_start = .;
        KEEP(*(.initcall4.init))
        KEEP(*(.initcall4s.init))
        __initcall5_start = .;
        KEEP(*(.initcall5.init))
        KEEP(*(.initcall5s.init))
        __initcall6_start = .;
        KEEP(*(.initcall6.init))
        KEEP(*(.initcall6s.init))
        __initcall7_start = .;
        KEEP(*(.initcall7.init))
        KEEP(*(.initcall7s.init))
        __initcall_end = .;
    } :text

    /* LinuxKPI module parameters and __setup/early_param handlers.  The
     * parser in linuxkpi/src/params.c walks these when applying the boot
     * command line.  KEEP is mandatory: --gc-sections would discard the
     * entries (and their referenced variables/functions) otherwise. */
    .kpi_params : ALIGN(8) {
        __kpi_param_start = .;
        KEEP(*(.kpi_params))
        __kpi_param_end = .;
    } :text

    .kpi_setups : ALIGN(8) {
        __kpi_setup_start = .;
        KEEP(*(.kpi_setups))
        __kpi_setup_end = .;
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __start___ex_table = .;
        KEEP(*(__ex_table))
        __stop___ex_table = .;
        . = ALIGN(8);
        __start___bug_table = .;
        KEEP(*(__bug_table))
        __stop___bug_table = .;
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
