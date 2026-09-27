#ifndef DTB_H
#define DTB_H

#include <stdint.h>
#include <stddef.h>

/* 
 * Flattened Device Tree (FDT) Header 
 * All values are big-endian in the blob!
 */
struct fdt_header {
    uint32_t magic;          // 0xd00dfeed
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_MAGIC 0xedfe0dd0 // 0xd00dfeed in Big Endian (swapped for little endian storage)

/* FDT Tokens */
#define FDT_BEGIN_NODE 0x01000000
#define FDT_END_NODE   0x02000000
#define FDT_PROP       0x03000000
#define FDT_NOP        0x04000000
#define FDT_END        0x09000000

// Initialize the device tree from a DTB blob in memory
void dm_parse_dtb(void *fdt_blob);

#endif
