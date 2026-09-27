#include "drivers/storage/ramdisk.h"
#include "drivers/storage/block.h"
#include "console/klog.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "lib/string.h"

// One ramdisk instance backed by a flat memory region.
struct ramdisk {
    uint8_t *base;       // virtual (HHDM) address of the image
    uint64_t size_bytes; // total byte size of the image
};

static int ramdisk_read(struct block_device *dev, uint64_t lba,
                        uint32_t count, void *buf)
{
    struct ramdisk *rd = (struct ramdisk *)dev->driver_data;
    uint64_t offset = lba * BLOCK_SECTOR_SIZE;
    uint64_t len    = (uint64_t)count * BLOCK_SECTOR_SIZE;

    if (offset >= rd->size_bytes)
        return -1;
    if (offset + len > rd->size_bytes)
        len = rd->size_bytes - offset;

    memcpy(buf, rd->base + offset, len);
    return 0;
}

static int ramdisk_write(struct block_device *dev, uint64_t lba,
                         uint32_t count, const void *buf)
{
    struct ramdisk *rd = (struct ramdisk *)dev->driver_data;
    uint64_t offset = lba * BLOCK_SECTOR_SIZE;
    uint64_t len    = (uint64_t)count * BLOCK_SECTOR_SIZE;

    if (offset >= rd->size_bytes)
        return -1;
    if (offset + len > rd->size_bytes)
        len = rd->size_bytes - offset;

    memcpy(rd->base + offset, buf, len);
    return 0;
}

void ramdisk_init(struct limine_module_response *modules)
{
    if (!modules || modules->module_count == 0) {
        klog_puts("[RAMDISK] No Limine modules provided.\n");
        return;
    }

    uint64_t hhdm = pmm_get_hhdm_offset();

    for (uint64_t i = 0; i < modules->module_count; i++) {
        struct limine_file *f = modules->modules[i];
        if (!f)
            continue;

        // Match the module by its path suffix "disk.img"
        const char *path = f->path ? f->path : "";
        uint64_t plen = strlen(path);
        const char *needle = "disk.img";
        uint64_t nlen = strlen(needle);
        int match = 0;
        if (plen >= nlen) {
            const char *tail = path + plen - nlen;
            if (strcmp(tail, needle) == 0)
                match = 1;
        }
        if (!match)
            continue;

        uint64_t phys = (uint64_t)f->address;
        uint64_t size = f->size;

        klog_puts("[RAMDISK] Found module: ");
        klog_puts(path);
        klog_puts(" phys=0x");
        klog_uint64(phys);
        klog_puts(" size=");
        klog_uint64(size / 1024 / 1024);
        klog_puts(" MiB\n");

        struct ramdisk *rd = kmalloc(sizeof(struct ramdisk));
        if (!rd) {
            klog_puts("[RAMDISK] Out of memory.\n");
            return;
        }

        // Limine maps module data in the HHDM, but f->address is already a
        // virtual pointer in the higher-half direct map.
        rd->base       = (uint8_t *)((uint64_t)f->address + hhdm);
        rd->size_bytes = size;

        // Sanity-check: if f->address already looks like a high address
        // (above 0xFFFF800000000000), it's already a HHDM pointer.
        if ((uint64_t)f->address >= 0xFFFF800000000000ULL) {
            rd->base = (uint8_t *)f->address;
        }

        struct block_device *bdev = kmalloc(sizeof(struct block_device));
        if (!bdev) {
            kfree(rd);
            klog_puts("[RAMDISK] Out of memory.\n");
            return;
        }
        memset(bdev, 0, sizeof(struct block_device));

        strncpy(bdev->name, "ram0", 15);
        bdev->sector_size    = BLOCK_SECTOR_SIZE;
        bdev->total_sectors  = size / BLOCK_SECTOR_SIZE;
        bdev->read_sectors   = ramdisk_read;
        bdev->write_sectors  = ramdisk_write;
        bdev->driver_data    = rd;

        block_register(bdev);

        klog_puts("[RAMDISK] Registered block device 'ram0' (");
        klog_uint64(bdev->total_sectors);
        klog_puts(" sectors)\n");
        return; // only one ramdisk needed
    }

    klog_puts("[RAMDISK] No 'disk.img' module found.\n");
}
