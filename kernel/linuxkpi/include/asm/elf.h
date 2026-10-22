#ifndef __AVORY_LINUXKPI_ASM_ELF_H
#define __AVORY_LINUXKPI_ASM_ELF_H
/* Firmware ELF metadata only; native exec owns process personality/regs. */
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <uapi/asm/elf.h>
#endif
#define ELF_CLASS ELFCLASS64
#define ELF_DATA ELFDATA2LSB
#define ELF_ARCH EM_X86_64
#endif
