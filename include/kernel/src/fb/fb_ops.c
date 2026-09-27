#include "framebuffer.h"
#include "fb.h"
#include "terminal.h"
#include "../fs/vfs.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../console/klog.h"

extern struct fb_info fb_global;

uint32_t fb_dev_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node;
    if (!buffer || !size || offset >= fb_global.screen_size)
        return 0;

    if (offset + size > fb_global.screen_size)
        size = (uint32_t)(fb_global.screen_size - offset);

    void *src_base = (fb_global.backbuffer_enabled && fb_global.backbuffer)
                         ? fb_global.backbuffer
                         : fb_global.screen_base;
    if (!src_base)
        return 0;

    memcpy(buffer, (const uint8_t *)src_base + offset, size);
    return size;
}

uint32_t fb_dev_write(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node;
    if (!buffer || !size || offset >= fb_global.screen_size)
        return 0;

    if (offset + size > fb_global.screen_size)
        size = (uint32_t)(fb_global.screen_size - offset);

    void *target = (fb_global.backbuffer_enabled && fb_global.backbuffer)
                       ? fb_global.backbuffer
                       : fb_global.screen_base;
    if (!target)
        return 0;

    uint8_t *dst = (uint8_t *)target + offset;
    const uint8_t *src = buffer;
    size_t count = size;

    while (count > 0 && (((uintptr_t)dst | (uintptr_t)src) & 7) != 0) {
        *dst++ = *src++;
        count--;
    }

    size_t qwords = count >> 3;
    uint64_t *d64 = (uint64_t *)dst;
    const uint64_t *s64 = (const uint64_t *)src;
    for (size_t q = 0; q < qwords; q++) {
        d64[q] = s64[q];
    }

    size_t rem = count & 7;
    if (rem) {
        uint8_t *drem = (uint8_t *)d64 + (qwords << 3);
        const uint8_t *srem = (const uint8_t *)s64 + (qwords << 3);
        for (size_t r = 0; r < rem; r++) {
            drem[r] = srem[r];
        }
    }

    uint32_t pitch = fb_global.fix.line_length;
    if (pitch) {
        uint32_t start_y = offset / pitch;
        uint32_t end_y = (offset + size + pitch - 1) / pitch;
        if (start_y < fb_global.var.yres) {
            uint32_t h = (end_y > fb_global.var.yres ? fb_global.var.yres : end_y) - start_y;
            if (h > 0) {
                fb_mark_dirty(0, start_y, fb_global.var.xres, h);
            }
        }
    }

    return size;
}

uint64_t fb_dev_mmap(struct vfs_node *node, uint64_t addr, uint64_t length,
                     uint64_t prot, uint64_t flags, uint64_t offset) {
    (void)node;
    (void)prot;
    (void)flags;

    if (offset >= fb_global.screen_size)
        return (uint64_t)-22; // -EINVAL

    if (offset + length > fb_global.screen_size)
        length = fb_global.screen_size - offset;

    uint64_t *user_pml4 = vmm_get_active_pml4();
    if (!user_pml4)
        return (uint64_t)-14; // -EFAULT

    uint64_t phys_base = fb_global.phys_base;
    if (!phys_base && fb_global.screen_base) {
        phys_base = vmm_virt_to_phys(user_pml4, (uint64_t)fb_global.screen_base);
        if (!phys_base && (uint64_t)fb_global.screen_base >= 0xFFFF800000000000ULL) {
            phys_base = (uint64_t)fb_global.screen_base - 0xFFFF800000000000ULL;
        }
    }

    if (!phys_base)
        return (uint64_t)-14;

    /* Write-Combining (WC) page attributes for user MMIO (PAT entry 7: PAT=1, PCD=1, PWT=1) */
    uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_USER |
                          PAGE_FLAG_PAT | PAGE_FLAG_PCD | PAGE_FLAG_PWT;

    size_t pages = (length + 0xFFF) / 0x1000;
    if (!vmm_map_range(user_pml4, addr, (phys_base + offset), pages, page_flags)) {
        return (uint64_t)-12; // -ENOMEM
    }

    return addr;
}

int fb_dev_ioctl(struct vfs_node *node, uint32_t cmd, uint64_t arg) {
    (void)node;

    switch (cmd) {
    case FBIOGET_VSCREENINFO: {
        if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct fb_var_screeninfo)))
            return -14; // -EFAULT
        memcpy((void *)arg, &fb_global.var, sizeof(struct fb_var_screeninfo));
        return 0;
    }
    case FBIOPUT_VSCREENINFO: {
        if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct fb_var_screeninfo)))
            return -14;
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        fb_global.var.xoffset = var->xoffset;
        fb_global.var.yoffset = var->yoffset;
        return 0;
    }
    case FBIOGET_FSCREENINFO: {
        if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct fb_fix_screeninfo)))
            return -14;
        memcpy((void *)arg, &fb_global.fix, sizeof(struct fb_fix_screeninfo));
        return 0;
    }
    case FBIOPAN_DISPLAY: {
        if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct fb_var_screeninfo)))
            return -14;
        struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
        fb_global.var.xoffset = var->xoffset;
        fb_global.var.yoffset = var->yoffset;
        return 0;
    }
    case FBIOBLANK: {
        if (arg == FB_BLANK_UNBLANK) {
            fb_global.blanked = false;
            fb_swap_buffer_rect(0, 0, fb_global.var.xres, fb_global.var.yres);
        } else {
            fb_global.blanked = true;
            if (fb_global.screen_base) {
                memset(fb_global.screen_base, 0, fb_global.screen_size);
            }
        }
        return 0;
    }
    case FBIOGETCMAP:
    case FBIOPUTCMAP:
    case FBIO_CURSOR:
    case FBIO_WAITFORVSYNC:
        return 0;

    case FBIODIRTYRECT: {
        if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(struct fb_copyarea)))
            return -14;
        struct fb_copyarea *area = (struct fb_copyarea *)arg;
        fb_mark_dirty(area->dx, area->dy, area->width, area->height);
        return 0;
    }

    /* ── KD / VT Console Modes ───────────────────────────────────────────── */
    case KDSETMODE: {
        fb_set_kd_mode((int)arg);
        return 0;
    }
    case KDGETMODE: {
        if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
            return -14;
        *(int *)arg = fb_get_kd_mode();
        return 0;
    }
    case KDGKBMODE: {
        if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
            return -14;
        *(int *)arg = 0;
        return 0;
    }
    case KDSKBMODE:
        return 0;

    case VT_OPENQRY: {
        if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
            return -14;
        *(int *)arg = 1;
        return 0;
    }
    case VT_GETMODE:
    case VT_SETMODE:
    case VT_GETSTATE:
    case VT_RELDISP:
    case VT_ACTIVATE:
    case VT_WAITACTIVE:
    case VT_DISALLOCATE:
        return 0;

    default:
        return -25; // -ENOTTY
    }
}

void fb_dev_open(struct vfs_node *node) {
    (void)node;
}

void fb_dev_close(struct vfs_node *node) {
    (void)node;
    if (fb_global.backbuffer_enabled && fb_global.is_dirty) {
        fb_swap_buffer();
    }
}

int fb_dev_poll(struct vfs_node *node, int events) {
    (void)node;
    return (events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM));
}

struct fb_ops fb_default_ops = {
    .fb_open = (int (*)(struct fb_info *, int))fb_dev_open,
    .fb_release = (int (*)(struct fb_info *, int))fb_dev_close,
    .fb_read = (uint32_t (*)(struct fb_info *, uint32_t, uint32_t, uint8_t *))fb_dev_read,
    .fb_write = (uint32_t (*)(struct fb_info *, uint32_t, uint32_t, const uint8_t *))fb_dev_write,
    .fb_fillrect = (void (*)(struct fb_info *, const struct fb_fillrect *))fb_fill_rect,
    .fb_copyarea = (void (*)(struct fb_info *, const struct fb_copyarea *))fb_copyarea,
    .fb_imageblit = (void (*)(struct fb_info *, const struct fb_image *))fb_imageblit,
    .fb_ioctl = (int (*)(struct fb_info *, uint32_t, uint64_t))fb_dev_ioctl,
};
