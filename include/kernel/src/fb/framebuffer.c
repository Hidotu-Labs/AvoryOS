#include "framebuffer.h"
#include "fb.h"
#include "terminal.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../drivers/input/keyboard.h"
#include "../fs/devfs.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../sched/wait.h"

struct fb_info fb_global;
static vfs_node_t fb_vfs_node;
static vfs_node_t fb_alias_node;
static vfs_node_t console_vfs_node;
static vfs_node_t tty0_vfs_node;
static vfs_node_t tty_vfs_node;

extern struct fb_ops fb_default_ops;
extern struct termios console_termios;

/* Forward declarations from fb_ops.c */
extern uint32_t fb_dev_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
extern uint32_t fb_dev_write(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
extern uint64_t fb_dev_mmap(struct vfs_node *node, uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset);
extern int fb_dev_ioctl(struct vfs_node *node, uint32_t cmd, uint64_t arg);
extern void fb_dev_open(struct vfs_node *node);
extern void fb_dev_close(struct vfs_node *node);
extern int fb_dev_poll(struct vfs_node *node, int events);

/* ── Console VFS Node Implementation ───────────────────────────────────────── */

static uint32_t console_dev_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    if (!buffer || !size)
        return 0;

    bool canonical = (console_termios.c_lflag & 0x00000002) != 0; // ICANON
    bool echo = (console_termios.c_lflag & 0x00000008) != 0;      // ECHO
    bool icrnl = (console_termios.c_iflag & 0x00000100) != 0;    // ICRNL

    if (!canonical) {
        /* Non-canonical (raw) mode */
        char c = keyboard_get_char();
        if (c == '\r' && icrnl)
            c = '\n';

        buffer[0] = (uint8_t)c;
        if (echo) {
            char ech[1] = {c};
            console_write_batch(ech, 1);
        }
        return 1;
    }

    /* Canonical (line-buffered) mode */
    uint32_t count = 0;
    while (count < size) {
        char c = keyboard_get_char();

        if (c == '\r' && icrnl)
            c = '\n';

        if (c == '\b' || c == 0x7F) {
            if (count > 0) {
                count--;
                if (echo) {
                    console_write_batch("\b \b", 3);
                }
            }
            continue;
        }

        if (c == 0x03) { // Ctrl-C (VINTR)
            if (echo) {
                console_write_batch("^C\r\n", 4);
            }
            buffer[0] = 0x03;
            return 1;
        }

        if (c == 0x04) { // Ctrl-D (VEOF)
            if (count == 0)
                return 0; // EOF
            break;
        }

        buffer[count++] = (uint8_t)c;

        if (echo) {
            if (c == '\n') {
                console_write_batch("\r\n", 2);
            } else {
                char ech[1] = {c};
                console_write_batch(ech, 1);
            }
        }

        if (c == '\n')
            break;
    }

    return count;
}

static uint32_t console_dev_write(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    if (!buffer || !size)
        return 0;

    console_write_batch((const char *)buffer, size);
    return size;
}

static int console_dev_poll(struct vfs_node *node, int events) {
    (void)node;
    int revents = 0x0004 | 0x0100; // POLLOUT | POLLWRNORM
    if (keyboard_has_char())
        revents |= 0x0001 | 0x0040; // POLLIN | POLLRDNORM
    return (events & revents);
}

static int console_dev_ioctl(struct vfs_node *node, uint32_t cmd, uint64_t arg) {
    (void)node;
    switch (cmd) {
    case 0x5413: { // TIOCGWINSZ
        struct winsize *ws = (struct winsize *)arg;
        if (!ws || !vmm_is_user_addr_range_valid(arg, sizeof(struct winsize)))
            return -14; // -EFAULT
        uint32_t w = fb_get_width();
        uint32_t h = fb_get_height();
        ws->ws_col = (unsigned short)(w ? w / 8 : 80);
        ws->ws_row = (unsigned short)(h ? h / 16 : 25);
        ws->ws_xpixel = (unsigned short)w;
        ws->ws_ypixel = (unsigned short)h;
        return 0;
    }
    case TCGETS: {
        struct kernel_termios *kt = (struct kernel_termios *)arg;
        if (!kt || !vmm_is_user_addr_range_valid(arg, sizeof(struct kernel_termios)))
            return -14;
        kt->c_iflag = console_termios.c_iflag;
        kt->c_oflag = console_termios.c_oflag;
        kt->c_cflag = console_termios.c_cflag;
        kt->c_lflag = console_termios.c_lflag;
        kt->c_line = console_termios.c_line;
        memcpy(kt->c_cc, console_termios.c_cc, KERNEL_NCCS);
        return 0;
    }
    case TCGETS2: {
        struct termios2 *t2 = (struct termios2 *)arg;
        if (!t2 || !vmm_is_user_addr_range_valid(arg, sizeof(struct termios2)))
            return -14;
        t2->c_iflag = console_termios.c_iflag;
        t2->c_oflag = console_termios.c_oflag;
        t2->c_cflag = console_termios.c_cflag;
        t2->c_lflag = console_termios.c_lflag;
        t2->c_line = console_termios.c_line;
        memcpy(t2->c_cc, console_termios.c_cc, KERNEL_NCCS);
        t2->c_ispeed = 38400;
        t2->c_ospeed = 38400;
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        const struct kernel_termios *kt = (const struct kernel_termios *)arg;
        if (!kt || !vmm_is_user_addr_range_valid(arg, sizeof(struct kernel_termios)))
            return -14;
        console_termios.c_iflag = kt->c_iflag;
        console_termios.c_oflag = kt->c_oflag;
        console_termios.c_cflag = kt->c_cflag;
        console_termios.c_lflag = kt->c_lflag;
        console_termios.c_line = kt->c_line;
        memcpy(console_termios.c_cc, kt->c_cc, KERNEL_NCCS);
        return 0;
    }
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2: {
        const struct termios2 *t2 = (const struct termios2 *)arg;
        if (!t2 || !vmm_is_user_addr_range_valid(arg, sizeof(struct termios2)))
            return -14;
        console_termios.c_iflag = t2->c_iflag;
        console_termios.c_oflag = t2->c_oflag;
        console_termios.c_cflag = t2->c_cflag;
        console_termios.c_lflag = t2->c_lflag;
        console_termios.c_line = t2->c_line;
        memcpy(console_termios.c_cc, t2->c_cc, KERNEL_NCCS);
        return 0;
    }
    case TIOCGPGRP: {
        int *pgrp = (int *)arg;
        if (!pgrp || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
            return -14;
        struct thread *t = sched_get_current();
        *pgrp = t ? (int)t->pgid : 1;
        return 0;
    }
    case TIOCSPGRP: {
        int *pgrp = (int *)arg;
        if (!pgrp || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
            return -14;
        return 0;
    }
    case KDSETMODE: {
        fb_set_kd_mode((int)arg);
        return 0;
    }
    case KDGETMODE: {
        int *mode_out = (int *)arg;
        if (!mode_out || !vmm_is_user_addr_range_valid(arg, sizeof(int)))
            return -14;
        *mode_out = fb_get_kd_mode();
        return 0;
    }
    case VT_OPENQRY:
    case VT_GETMODE:
    case VT_SETMODE:
    case VT_GETSTATE:
    case VT_ACTIVATE:
    case VT_WAITACTIVE:
    case VT_RELDISP:
    case VT_DISALLOCATE:
        return 0;
    default:
        return -25; // -ENOTTY
    }
}

static void console_dev_open(struct vfs_node *node) { (void)node; }
static void console_dev_close(struct vfs_node *node) { (void)node; }

/* ── Device Registry ─────────────────────────────────────────────────────── */
#define MAX_FB_DEVICES 256
typedef struct {
    char name[64];
    vfs_node_t *node;
} fb_device_entry_t;

static fb_device_entry_t fb_device_registry[MAX_FB_DEVICES];
static size_t fb_device_count = 0;
static spinlock_t fb_registry_lock = SPINLOCK_INIT;

void fb_register_device_node(const char *name, vfs_node_t *node) {
    if (!name || !node)
        return;

    spinlock_acquire(&fb_registry_lock);
    for (size_t i = 0; i < fb_device_count; i++) {
        if (strcmp(fb_device_registry[i].name, name) == 0) {
            fb_device_registry[i].node = node;
            spinlock_release(&fb_registry_lock);
            devfs_register_node(name, node);
            return;
        }
    }

    if (fb_device_count < MAX_FB_DEVICES) {
        strncpy(fb_device_registry[fb_device_count].name, name, 63);
        fb_device_registry[fb_device_count].name[63] = '\0';
        fb_device_registry[fb_device_count].node = node;
        fb_device_count++;
    }
    spinlock_release(&fb_registry_lock);

    devfs_register_node(name, node);
}

vfs_node_t *fb_lookup_device(const char *name) {
    if (!name)
        return NULL;

    if (strcmp(name, "console") == 0 || strcmp(name, "/dev/console") == 0 ||
        strcmp(name, "dev/console") == 0) {
        return &console_vfs_node;
    }
    if (strcmp(name, "tty0") == 0 || strcmp(name, "/dev/tty0") == 0 ||
        strcmp(name, "dev/tty0") == 0 || strcmp(name, "tty") == 0 ||
        strcmp(name, "/dev/tty") == 0 || strcmp(name, "dev/tty") == 0) {
        return &console_vfs_node;
    }

    if (strcmp(name, "fb0") == 0 || strcmp(name, "fb") == 0 ||
        strcmp(name, "framebuffer") == 0 || strcmp(name, "/dev/fb0") == 0 ||
        strcmp(name, "dev/fb0") == 0) {
        if (fb_global.vfs_node)
            return fb_global.vfs_node;
        return &fb_vfs_node;
    }

    spinlock_acquire(&fb_registry_lock);
    for (size_t i = 0; i < fb_device_count; i++) {
        if (strcmp(fb_device_registry[i].name, name) == 0) {
            vfs_node_t *n = fb_device_registry[i].node;
            spinlock_release(&fb_registry_lock);
            return n;
        }
    }
    spinlock_release(&fb_registry_lock);

    return devfs_lookup(name);
}

/* ── Framebuffer Core Initialization ─────────────────────────────────────── */

void fb_init(struct limine_framebuffer *framebuffer) {
    if (!framebuffer)
        return;

    void *saved_backbuffer = fb_global.backbuffer;
    bool saved_backbuffer_enabled = fb_global.backbuffer_enabled;

    memset(&fb_global, 0, sizeof(struct fb_info));
    spinlock_init(&fb_global.lock);

    fb_global.node = 0;
    fb_global.screen_base = (void *)framebuffer->address;
    fb_global.var.xres = (uint32_t)framebuffer->width;
    fb_global.var.yres = (uint32_t)framebuffer->height;
    fb_global.var.xres_virtual = (uint32_t)framebuffer->width;
    fb_global.var.yres_virtual = (uint32_t)framebuffer->height;
    fb_global.var.bits_per_pixel = (uint32_t)framebuffer->bpp;
    fb_global.fix.line_length = (uint32_t)framebuffer->pitch;

    fb_global.screen_size = (uint64_t)framebuffer->height * framebuffer->pitch;
    fb_global.fix.smem_len = (uint32_t)fb_global.screen_size;
    fb_global.fix.type = FB_TYPE_PACKED_PIXELS;
    fb_global.fix.visual = FB_VISUAL_TRUECOLOR;
    fb_global.fix.accel = FB_ACCEL_NONE;

    strncpy(fb_global.fix.id, "AvoryFB", 15);
    fb_global.fix.id[15] = '\0';

    fb_global.var.red.offset = framebuffer->red_mask_shift;
    fb_global.var.red.length = framebuffer->red_mask_size;
    fb_global.var.green.offset = framebuffer->green_mask_shift;
    fb_global.var.green.length = framebuffer->green_mask_size;
    fb_global.var.blue.offset = framebuffer->blue_mask_shift;
    fb_global.var.blue.length = framebuffer->blue_mask_size;

    fb_global.kd_mode = KD_TEXT;
    fb_global.fbops = &fb_default_ops;

    /* Derive physical base from the HHDM address Limine handed us.  The real
     * offset from pmm is used rather than the usual 0xFFFF8000... constant so
     * the WC remap below targets the right frames. */
    uint64_t hhdm = pmm_get_hhdm_offset();
    uint64_t fb_addr = (uint64_t)framebuffer->address;
    if (hhdm && fb_addr >= hhdm) {
        fb_global.phys_base = fb_addr - hhdm;
    } else if (fb_addr >= 0xFFFF800000000000ULL) {
        fb_global.phys_base = fb_addr - 0xFFFF800000000000ULL;
    } else {
        fb_global.phys_base = hhdm ? 0 : fb_addr;
    }
    fb_global.fix.smem_start = fb_global.phys_base;

    fb_global.backbuffer = saved_backbuffer;
    fb_global.backbuffer_enabled = saved_backbuffer_enabled;
    fb_global.is_dirty = false;

    /* The aperture is mapped long before vmm_init(); retype it once the
     * kernel page tables exist so every screen_base user gets WC stores. */
    if (vmm_get_kernel_pml4())
        fb_map_wc();
}

/* ── Write-Combining Aperture Mapping ────────────────────────────────────── */

static bool fb_wc_mapped = false;

/*
 * The Limine HHDM mapping of VRAM inherits whatever memory type firmware left
 * behind, which is uncached on most machines.  Uncached qword stores are one
 * bus transaction each, so a full-screen console blit costs milliseconds.
 *
 * Re-type the kernel's mapping of the visible aperture in place as
 * write-combining (PAT entry 7, programmed in cpu_pat_init()) instead of
 * creating an alias: screen_base does not change, so the swap, the DRM
 * software fallback and klog all pick the new type up for free.  vmm_map_page()
 * flushes a replaced mapping, so no stale UC translation survives on this CPU,
 * and the call runs before the APs and userland exist to hold one.
 *
 * WC versus MTRR: a firmware MTRR that already marks the range UC wins over
 * PAT.  In that case this is harmless but the win comes from the non-temporal
 * stores in fb_swap_buffer_rect() instead.
 */
void fb_map_wc(void) {
    if (fb_wc_mapped)
        return;

    uint64_t *pml4 = vmm_get_kernel_pml4();
    if (!pml4 || !fb_global.screen_base || !fb_global.screen_size)
        return;

    uint64_t phys = fb_global.phys_base;
    if (!phys)
        phys = vmm_virt_to_phys(pml4, (uint64_t)fb_global.screen_base) & PAGE_MASK;
    if (!phys)
        return;

    uint64_t va = (uint64_t)fb_global.screen_base;
    size_t pages = (fb_global.screen_size + 0xFFF) / 0x1000;
    /* PAT=1, PCD=1, PWT=1 selects PAT entry 7 (WC).  RW only: this is a kernel
     * mapping, userland gets its own WC VMA through fb_dev_mmap(). */
    uint64_t flags = PAGE_FLAG_RW | PAGE_FLAG_PAT | PAGE_FLAG_PCD | PAGE_FLAG_PWT;

    for (size_t i = 0; i < pages; i++) {
        if (!vmm_map_page(pml4, va + i * 0x1000, phys + i * 0x1000, flags)) {
            klog_puts("[FB] Warning: WC remap stopped at page ");
            klog_uint64(i);
            klog_puts("; earlier pages stay write-combining\n");
            return;
        }
    }

    fb_wc_mapped = true;
    klog_puts("[FB] Framebuffer aperture remapped write-combining: ");
    klog_uint64(pages);
    klog_puts(" pages\n");
}

/* ── Backbuffer Management ───────────────────────────────────────────────── */

static bool fb_ensure_backbuffer(void) {
    if (fb_global.backbuffer)
        return true;

    if (!fb_global.screen_size)
        return false;

    void *buf = kmalloc(fb_global.screen_size);
    if (!buf)
        return false;

    memset(buf, 0, fb_global.screen_size);
    fb_global.backbuffer = buf;
    return true;
}

void fb_set_backbuffer_mode(bool enabled) {
    if (enabled) {
        if (fb_ensure_backbuffer()) {
            fb_global.backbuffer_enabled = true;
        } else {
            fb_global.backbuffer_enabled = false;
        }
    } else {
        fb_global.backbuffer_enabled = false;
    }
}

bool fb_is_backbuffer_enabled(void) {
    return fb_global.backbuffer_enabled && fb_global.backbuffer != NULL;
}

void *fb_get_backbuffer(void) {
    return fb_global.backbuffer;
}

void *fb_get_target_buffer(void) {
    if (fb_global.backbuffer_enabled && fb_global.backbuffer)
        return fb_global.backbuffer;
    return fb_global.screen_base;
}

/* ── Kernel Getters & Setters ────────────────────────────────────────────── */

uint32_t fb_get_width(void) {
    return fb_global.var.xres;
}

uint32_t fb_get_height(void) {
    return fb_global.var.yres;
}

uint32_t fb_get_pitch(void) {
    return fb_global.fix.line_length;
}

uint32_t fb_get_bpp(void) {
    return fb_global.var.bits_per_pixel;
}

void *fb_get_base(void) {
    return fb_global.screen_base;
}

int fb_get_kd_mode(void) {
    return fb_global.kd_mode;
}

void fb_set_kd_mode(int mode) {
    fb_global.kd_mode = mode;
}

/* ── VFS DevFS Registration ──────────────────────────────────────────────── */

void fb_register_vfs(void) {
    /* Setup Framebuffer character device */
    vfs_node_init(&fb_vfs_node);
    strncpy(fb_vfs_node.name, "fb0", 127);
    fb_vfs_node.flags = FS_CHARDEV | FS_PERSISTENT;
    fb_vfs_node.mask = 0666;
    fb_vfs_node.length = (uint32_t)fb_global.screen_size;
    fb_vfs_node.read = fb_dev_read;
    fb_vfs_node.write = fb_dev_write;
    fb_vfs_node.mmap = fb_dev_mmap;
    fb_vfs_node.ioctl = fb_dev_ioctl;
    fb_vfs_node.open = fb_dev_open;
    fb_vfs_node.close = fb_dev_close;
    fb_vfs_node.poll = fb_dev_poll;

    vfs_node_init(&fb_alias_node);
    strncpy(fb_alias_node.name, "fb", 127);
    fb_alias_node.flags = FS_CHARDEV | FS_PERSISTENT;
    fb_alias_node.mask = 0666;
    fb_alias_node.length = (uint32_t)fb_global.screen_size;
    fb_alias_node.read = fb_dev_read;
    fb_alias_node.write = fb_dev_write;
    fb_alias_node.mmap = fb_dev_mmap;
    fb_alias_node.ioctl = fb_dev_ioctl;
    fb_alias_node.open = fb_dev_open;
    fb_alias_node.close = fb_dev_close;
    fb_alias_node.poll = fb_dev_poll;

    fb_global.vfs_node = &fb_vfs_node;
    devfs_register_node("fb0", &fb_vfs_node);
    devfs_register_node("fb", &fb_alias_node);

    /* Setup Console / TTY character devices */
    vfs_node_init(&console_vfs_node);
    strncpy(console_vfs_node.name, "console", 127);
    console_vfs_node.flags = FS_CHARDEV | FS_PERSISTENT;
    console_vfs_node.mask = 0666;
    console_vfs_node.read = console_dev_read;
    console_vfs_node.write = console_dev_write;
    console_vfs_node.ioctl = console_dev_ioctl;
    console_vfs_node.poll = console_dev_poll;
    console_vfs_node.open = console_dev_open;
    console_vfs_node.close = console_dev_close;

    vfs_node_init(&tty0_vfs_node);
    strncpy(tty0_vfs_node.name, "tty0", 127);
    tty0_vfs_node.flags = FS_CHARDEV | FS_PERSISTENT;
    tty0_vfs_node.mask = 0666;
    tty0_vfs_node.read = console_dev_read;
    tty0_vfs_node.write = console_dev_write;
    tty0_vfs_node.ioctl = console_dev_ioctl;
    tty0_vfs_node.poll = console_dev_poll;
    tty0_vfs_node.open = console_dev_open;
    tty0_vfs_node.close = console_dev_close;

    vfs_node_init(&tty_vfs_node);
    strncpy(tty_vfs_node.name, "tty", 127);
    tty_vfs_node.flags = FS_CHARDEV | FS_PERSISTENT;
    tty_vfs_node.mask = 0666;
    tty_vfs_node.read = console_dev_read;
    tty_vfs_node.write = console_dev_write;
    tty_vfs_node.ioctl = console_dev_ioctl;
    tty_vfs_node.poll = console_dev_poll;
    tty_vfs_node.open = console_dev_open;
    tty_vfs_node.close = console_dev_close;

    devfs_register_node("console", &console_vfs_node);
    devfs_register_node("tty0", &tty0_vfs_node);
    devfs_register_node("tty", &tty_vfs_node);

    fb_register_device_node("console", &console_vfs_node);
    fb_register_device_node("tty0", &tty0_vfs_node);
    fb_register_device_node("tty", &tty_vfs_node);

    klog_puts("[FB] Registered /dev/fb0, /dev/fb, /dev/console, /dev/tty0, and /dev/tty devices\n");
}

/* ── DRM Backend Detection ───────────────────────────────────────────────── */

void fb_detect_drm_backend(void) {
    if (fb_global.var.xres == 0 || fb_global.var.yres == 0)
        return;

    klog_puts("[FB] Linux fbdev subsystem initialized: ");
    klog_uint64(fb_global.var.xres);
    klog_puts("x");
    klog_uint64(fb_global.var.yres);
    klog_puts(" @ ");
    klog_uint64(fb_global.var.bits_per_pixel);
    klog_puts("bpp, pitch=");
    klog_uint64(fb_global.fix.line_length);
    klog_puts(" bytes\n");
}