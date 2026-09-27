#include "drivers/manager/dtb.h"
#include "drivers/manager/device.h"
#include "console/klog.h"
#include "lib/string.h"

static uint32_t fdt_swap32(uint32_t val) {
    return ((val << 24) & 0xFF000000) |
           ((val <<  8) & 0x00FF0000) |
           ((val >>  8) & 0x0000FF00) |
           ((val >> 24) & 0x000000FF);
}

void dm_parse_dtb(void *fdt_blob) {
    if (!fdt_blob) return;

    struct fdt_header *header = (struct fdt_header *)fdt_blob;
    if (fdt_swap32(header->magic) != 0xd00dfeed) {
        klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " DTB: Invalid magic - not a valid DTB blob.\n");
        return;
    }

    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " DTB: Found valid blob. Parsing...\n");

    uint32_t *p = (uint32_t *)((uint8_t *)fdt_blob + fdt_swap32(header->off_dt_struct));
    const char *str_tab = (const char *)((uint8_t *)fdt_blob + fdt_swap32(header->off_dt_strings));
    
    struct device *current_parent = device_find_by_path("/");
    
    while (fdt_swap32(*p) != FDT_END) {
        uint32_t token = fdt_swap32(*p++);

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            if (*name == '\0') name = "root";

            // Advance p past the name (null-terminated, aligned to 4 bytes)
            size_t len = strlen(name);
            p += (len + 4) / 4;

            struct device *new_dev = device_create(current_parent, name);
            if (new_dev) {
                current_parent = new_dev;
            }
        } else if (token == FDT_END_NODE) {
            if (current_parent && current_parent->parent) {
                current_parent = current_parent->parent;
            }
        } else if (token == FDT_PROP) {
            uint32_t data_len = fdt_swap32(*p++);
            uint32_t name_off = fdt_swap32(*p++);
            const char *prop_name = str_tab + name_off;
            (void)prop_name; /* unused for now */

            /* 
             * For now, we don't do much with properties, but we could 
             * extract "reg" for resources.
             */
            
            // Advance p past the property data (aligned to 4 bytes)
            p += (data_len + 3) / 4;
        } else if (token == FDT_NOP) {
            // Skip
        }
    }

    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " DTB: Parsing complete.\n");
}
