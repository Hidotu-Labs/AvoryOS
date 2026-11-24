#include "drivers/storage/ramdisk.h"
#include "apic/lapic_timer.h"
#include "drivers/storage/block.h"
#include "console/klog.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/zram/lz4.h"
#include "lib/string.h"

#define LZ4_FRAME_MAGIC 0x184D2204U

// One ramdisk instance backed either by a flat region or LZ4-decompressed chunks.
struct ramdisk {
    uint8_t *base;       // flat virtual (HHDM) address, or NULL when chunked
    uint8_t **chunks;    // chunk HHDM pointers when LZ4-decompressed
    uint32_t chunk_size; // bytes per chunk (LZ4 block max size)
    uint32_t num_chunks;
    uint64_t size_bytes; // total uncompressed byte size of the image
};

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p)
{
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

static int ramdisk_rw(struct ramdisk *rd, uint64_t offset, uint64_t len,
                      void *buf, bool write)
{
    if (!rd->chunks) {
        if (write)
            memcpy(rd->base + offset, buf, len);
        else
            memcpy(buf, rd->base + offset, len);
        return 0;
    }

    uint8_t *ubuf = (uint8_t *)buf;
    uint64_t off = offset;
    uint64_t rem = len;
    while (rem > 0) {
        uint32_t idx = (uint32_t)(off / rd->chunk_size);
        uint32_t in_chunk = (uint32_t)(off % rd->chunk_size);
        uint32_t room = rd->chunk_size - in_chunk;
        size_t chunk_len = rem < room ? (size_t)rem : (size_t)room;
        if (idx >= rd->num_chunks || !rd->chunks[idx])
            return -1;
        if (write)
            memcpy(rd->chunks[idx] + in_chunk, ubuf, chunk_len);
        else
            memcpy(ubuf, rd->chunks[idx] + in_chunk, chunk_len);
        ubuf += chunk_len;
        off += chunk_len;
        rem -= chunk_len;
    }
    return 0;
}

static int ramdisk_read(struct block_device *dev, uint64_t lba,
                        uint32_t count, void *buf)
{
    struct ramdisk *rd = (struct ramdisk *)dev->driver_data;
    uint64_t offset = lba * BLOCK_SECTOR_SIZE;
    uint64_t len    = (uint64_t)count * BLOCK_SECTOR_SIZE;

    if (offset >= rd->size_bytes)
        return -1;
    if (len > rd->size_bytes - offset)
        len = rd->size_bytes - offset;

    if (ramdisk_rw(rd, offset, len, buf, false) != 0)
        return -1;
    block_stat_read(dev, (uint32_t)(len / BLOCK_SECTOR_SIZE));
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
    if (len > rd->size_bytes - offset)
        len = rd->size_bytes - offset;

    if (ramdisk_rw(rd, offset, len, (void *)buf, true) != 0)
        return -1;
    block_stat_write(dev, (uint32_t)(len / BLOCK_SECTOR_SIZE));
    return 0;
}

/* Returns 1 if LZ4-decompressed into rd->chunks, 0 if src is not an LZ4 frame,
 * or -1 on malformed LZ4 data / OOM. */
static int ramdisk_try_decompress_lz4(struct ramdisk *rd, const uint8_t *src,
                                      uint64_t src_size, uint64_t hhdm)
{
    if (src_size < 15 || rd_le32(src) != LZ4_FRAME_MAGIC)
        return 0;

    uint8_t flg = src[4];
    uint8_t bd  = src[5];
    if (((flg >> 6) & 3U) != 1U || !(flg & 0x20U) || !(flg & 0x08U)) {
        klog_puts("[RAMDISK] Unsupported LZ4 frame flags\n");
        return -1;
    }

    uint32_t block_max_size = 0;
    switch ((bd >> 4) & 7U) {
    case 4: block_max_size = 65536U; break;
    case 5: block_max_size = 262144U; break;
    case 6: block_max_size = 1048576U; break;
    case 7: block_max_size = 4194304U; break;
    default:
        klog_puts("[RAMDISK] Unsupported LZ4 block size\n");
        return -1;
    }

    uint64_t content_size = rd_le64(src + 6);
    if (content_size == 0 || (block_max_size % PAGE_SIZE) != 0) {
        klog_puts("[RAMDISK] Invalid LZ4 content/block size\n");
        return -1;
    }

    uint64_t pos = 14;
    if (flg & 0x01U) { /* DictID */
        if (pos + 4 > src_size)
            return -1;
        pos += 4;
    }
    if (pos + 1 > src_size)
        return -1;
    pos += 1; /* Header Checksum */

    uint64_t chunks_u64 = (content_size + block_max_size - 1) / block_max_size;
    if (chunks_u64 > 0xFFFFFFFFULL)
        return -1;
    uint32_t num_chunks = (uint32_t)chunks_u64;
    size_t table_bytes = (size_t)num_chunks * sizeof(uint8_t *);
    uint8_t **chunks = kmalloc(table_bytes);
    if (!chunks) {
        klog_puts("[RAMDISK] Out of memory for LZ4 chunk table\n");
        return -1;
    }
    memset(chunks, 0, table_bytes);

    size_t chunk_pages = block_max_size / PAGE_SIZE;
    uint32_t chunk_idx = 0;
    uint64_t total_out = 0;

    for (;;) {
        if (pos + 4 > src_size)
            goto fail;
        uint32_t blk_hdr = rd_le32(src + pos);
        pos += 4;
        if (blk_hdr == 0)
            break;
        if (chunk_idx >= num_chunks)
            goto fail;

        void *phys = pmm_alloc_pages(chunk_pages);
        if (!phys)
            goto fail;
        uint8_t *chunk = (uint8_t *)((uint64_t)phys + hhdm);
        chunks[chunk_idx] = chunk;

        uint64_t rem_out = content_size - total_out;
        size_t expected_max = rem_out < block_max_size
                                  ? (size_t)rem_out
                                  : (size_t)block_max_size;
        size_t out_len = 0;

        if (blk_hdr & 0x80000000U) {
            uint32_t raw_len = blk_hdr & 0x7FFFFFFFU;
            if (raw_len == 0 || raw_len > expected_max || raw_len > src_size - pos)
                goto fail;
            memcpy(chunk, src + pos, raw_len);
            pos += raw_len;
            out_len = raw_len;
        } else {
            uint32_t comp_len = blk_hdr;
            if (comp_len == 0 || comp_len > src_size - pos)
                goto fail;
            if (zram_lz4_decompress_partial(src + pos, comp_len, chunk,
                                            expected_max, &out_len) != 0)
                goto fail;
            pos += comp_len;
        }

        if (flg & 0x10U) { /* Block Checksum */
            if (pos + 4 > src_size)
                goto fail;
            pos += 4;
        }

        if (total_out + out_len != content_size && out_len != block_max_size)
            goto fail;

        total_out += out_len;
        chunk_idx++;
    }

    if (total_out != content_size)
        goto fail;

    rd->base       = NULL;
    rd->chunks     = chunks;
    rd->chunk_size = block_max_size;
    rd->num_chunks = num_chunks;
    rd->size_bytes = content_size;
    return 1;

fail:
    for (uint32_t j = 0; j < num_chunks; j++) {
        if (chunks[j])
            pmm_free_pages((void *)((uint64_t)chunks[j] - hhdm), chunk_pages);
    }
    kfree(chunks);
    klog_puts("[RAMDISK] LZ4 decompression failed\n");
    return -1;
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

        uint8_t *src_virt = (uint64_t)f->address >= 0xFFFF800000000000ULL
                                ? (uint8_t *)f->address
                                : (uint8_t *)((uint64_t)f->address + hhdm);
        uint64_t src_phys = (uint64_t)src_virt - hhdm;
        uint64_t size     = f->size;

        klog_puts("[RAMDISK] Found module: ");
        klog_puts(path);
        klog_puts(" phys=0x");
        klog_hex64(src_phys);
        klog_puts(" size=");
        klog_uint64(size / 1024 / 1024);
        klog_puts(" MiB\n");

        struct ramdisk *rd = kmalloc(sizeof(struct ramdisk));
        if (!rd) {
            klog_puts("[RAMDISK] Out of memory.\n");
            return;
        }
        memset(rd, 0, sizeof(*rd));

        uint64_t t0 = lapic_timer_get_ms();
        int lz4_rc = ramdisk_try_decompress_lz4(rd, src_virt, size, hhdm);
        if (lz4_rc < 0) {
            kfree(rd);
            return;
        }
        if (lz4_rc == 1) {
            uint64_t elapsed = lapic_timer_get_ms() - t0;
            uint64_t freed = pmm_reclaim_region(src_phys, size);
            klog_puts("[RAMDISK] Decompressed LZ4 disk.img: ");
            klog_uint64(size / 1024 / 1024);
            klog_puts(" MiB -> ");
            klog_uint64(rd->size_bytes / 1024 / 1024);
            klog_puts(" MiB in ");
            klog_uint64(elapsed);
            klog_puts(" ms (reclaimed ");
            klog_uint64(freed / 1024 / 1024);
            klog_puts(" MiB)\n");
        } else {
            rd->base       = src_virt;
            rd->size_bytes = size;
        }

        struct block_device *bdev = kmalloc(sizeof(struct block_device));
        if (!bdev) {
            if (rd->chunks) {
                size_t chunk_pages = rd->chunk_size / PAGE_SIZE;
                for (uint32_t j = 0; j < rd->num_chunks; j++) {
                    if (rd->chunks[j])
                        pmm_free_pages((void *)((uint64_t)rd->chunks[j] - hhdm),
                                       chunk_pages);
                }
                kfree(rd->chunks);
            }
            kfree(rd);
            klog_puts("[RAMDISK] Out of memory.\n");
            return;
        }
        memset(bdev, 0, sizeof(struct block_device));

        strncpy(bdev->name, "ram0", 15);
        bdev->sector_size    = BLOCK_SECTOR_SIZE;
        bdev->total_sectors  = rd->size_bytes / BLOCK_SECTOR_SIZE;
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
