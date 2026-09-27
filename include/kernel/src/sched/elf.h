#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define ELF_MAGIC 0x464C457F

typedef struct {
  uint8_t e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define ET_EXEC 2
#define ET_DYN 3

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_SHLIB 5
#define PT_PHDR 6
#define PT_TLS 7

#define PF_X 1
#define PF_W 2
#define PF_R 4

// ELF Auxiliary Vector Types (for initial user stack)
#define AT_NULL 0      // End of auxiliary vector
#define AT_PHDR 3      // Program headers virtual address
#define AT_PHENT 4     // Size of program header entry
#define AT_PHNUM 5     // Number of program headers
#define AT_PAGESZ 6    // System page size
#define AT_ENTRY 9     // Entry point of program
#define AT_UID 11      // Real UID
#define AT_EUID 12     // Effective UID
#define AT_GID 13      // Real GID
#define AT_EGID 14     // Effective GID
#define AT_PLATFORM 15 // Platform string
#define AT_HWCAP 16    // Hardware capabilities
#define AT_CLKTCK 17   // Clock frequency
#define AT_SECURE 23   // Secure mode (suid)
#define AT_BASE 7      // Base address of interpreter
#define AT_FLAGS 8     // Flags
#define AT_RANDOM 25   // Address of 16 random bytes
#define AT_EXECFN 31   // Executable filename string address
#define AT_SYSINFO_EHDR 33 // vDSO ELF header address

// ELF metadata passed from loader to stack builder
typedef struct {
  uint64_t entry;        // e_entry
  uint64_t load_base;    // Load bias for ET_DYN main executables
  uint64_t phdr;         // virtual address of program headers
  uint16_t phentsize;    // e_phentsize
  uint16_t phnum;        // e_phnum
  uint64_t interp_base;  // Base address of interpreter
  uint64_t interp_entry; // Entry point of interpreter
} elf_info_t;

#endif
