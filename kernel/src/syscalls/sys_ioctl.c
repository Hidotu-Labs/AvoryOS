// sys_ioctl.c — ioctl syscall
#include "../console/klog.h"
#include "../fb/framebuffer.h"
#include "../drivers/pty.h"
#include "../font/font.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include <stdint.h>

#ifndef IOCTL_DEBUG_LOGGING
#define IOCTL_DEBUG_LOGGING 0
#endif

struct termios console_termios;

/* Keep one record per (device node, ioctl request), so GUI libraries that
 * repeatedly probe an unsupported ioctl produce a useful first report and
 * occasional counts instead of flooding serial output. */
#define IOCTL_MISS_SLOTS 128
struct ioctl_miss_record {
  uint32_t request;
  uint32_t node_hash;
  uint64_t calls;
  bool used;
};
static struct ioctl_miss_record ioctl_misses[IOCTL_MISS_SLOTS];
static spinlock_t ioctl_misses_lock = SPINLOCK_INIT;
static uint64_t ioctl_miss_overflow_calls;

static uint32_t ioctl_node_hash(const char *name) {
  uint32_t hash = 2166136261u;
  if (!name)
    return hash;
  while (*name) {
    hash = (hash ^ (uint8_t)*name++) * 16777619u;
  }
  return hash;
}

static const char *ioctl_request_name(uint32_t request) {
  switch (request) {
  case 0x0301: return "HDIO_GETGEO";
  case 0x540B: return "TCFLSH";
  case 0x5414: return "TIOCSWINSZ";
  case 0x541B: return "FIONREAD/TIOCINQ";
  case 0x1260: return "BLKGETSIZE";
  case 0x1268: return "BLKSSZGET";
  case 0x1272:
  case 0x80081272: return "BLKGETSIZE64";
  case 0x8912: return "SIOCGIFCONF";
  case 0x8915: return "SIOCGIFADDR";
  case 0x8933: return "SIOCGIFINDEX";
  case 0xC0186449: return "DRM_IOCTL_VIRTGPU_GET_CAPS";
  case 0xC0106443: return "DRM_IOCTL_VIRTGPU_GETPARAM";
  case 0xC01064C7: return "DRM_IOCTL_MODE_LIST_LESSEES";
  case 0x40049409: return "FICLONE/BTRFS_IOC_CLONE";
  case 0x4020940D: return "FICLONERANGE";
  default: return "unknown";
  }
}

static bool ioctl_miss_should_log(const char *node_name, uint32_t request,
                                 uint64_t *calls_out) {
  uint32_t node_hash = ioctl_node_hash(node_name);
  bool should_log = false;
  spinlock_acquire(&ioctl_misses_lock);
  struct ioctl_miss_record *free_record = NULL;
  for (size_t i = 0; i < IOCTL_MISS_SLOTS; i++) {
    struct ioctl_miss_record *record = &ioctl_misses[i];
    if (record->used && record->request == request &&
        record->node_hash == node_hash) {
      record->calls++;
      *calls_out = record->calls;
      should_log = record->calls == 1 || (record->calls % 256) == 0;
      spinlock_release(&ioctl_misses_lock);
      return should_log;
    }
    if (!record->used && !free_record)
      free_record = record;
  }
  if (free_record) {
    free_record->used = true;
    free_record->request = request;
    free_record->node_hash = node_hash;
    free_record->calls = 1;
    *calls_out = 1;
    should_log = true;
  } else {
    ioctl_miss_overflow_calls++;
    *calls_out = ioctl_miss_overflow_calls;
    should_log = ioctl_miss_overflow_calls == 1 ||
                 (ioctl_miss_overflow_calls % 256) == 0;
  }
  spinlock_release(&ioctl_misses_lock);
  return should_log;
}

static void ioctl_log_unhandled(struct thread *thread, uint64_t fd,
                                uint32_t request, uint64_t arg,
                                vfs_node_t *node, bool handler_called) {
  /* Xorg probes evdev descriptors with TCFLSH. It is a terminal-only ioctl,
   * and the evdev handler intentionally returns ENOTTY for this probe. */
  if (request == 0x540B && node &&
      (node->flags & FS_TYPE_MASK) == FS_CHARDEV &&
      (strcmp(node->name, "event0") == 0 ||
       strcmp(node->name, "event1") == 0))
    return;

  uint64_t calls = 0;
  if (!ioctl_miss_should_log(node ? node->name : NULL, request, &calls))
    return;

  klog_puts("[IOCTL-MISS] ");
  if (thread) {
    klog_puts("comm=");
    klog_puts(thread->comm);
    klog_puts(" tid=");
    klog_uint64(thread->tid);
    klog_puts(" tgid=");
    klog_uint64(thread->tgid);
    klog_puts(" ");
  }
  klog_puts("fd=");
  klog_uint64(fd);
  klog_puts(" node=");
  klog_puts(node && node->name[0] ? node->name : "<anonymous>");
  if (node) {
    klog_puts(" node_type=");
    klog_uint64(node->flags & FS_TYPE_MASK);
  }
  if (thread && thread->fd_paths && fd < MAX_FDS && thread->fd_paths[fd]) {
    klog_puts(" path=");
    klog_puts(thread->fd_paths[fd]->value);
  }
  klog_puts(" request=");
  klog_hex32(request);
  klog_puts(" name=");
  klog_puts(ioctl_request_name(request));
  klog_puts(" type=");
  klog_hex32((request >> 8) & 0xff);
  klog_puts(" nr=");
  klog_hex32(request & 0xff);
  klog_puts(" dir=");
  klog_uint64((request >> 30) & 3);
  klog_puts(" size=");
  klog_uint64((request >> 16) & 0x3fff);
  klog_puts(" arg=");
  klog_hex64(arg);
  klog_puts(" result=-ENOTTY handler=");
  klog_puts(handler_called ? "present-returned-ENOTTY" : "absent");
  klog_puts(" occurrences=");
  klog_uint64(calls);
  klog_puts("\n");
}

static int ioctl_arg_is_scalar(uint32_t request) {
  switch (request) {
  case 0x40044590: // EVIOCGRAB: _IOW('E', 0x90, int)
  case 0x40044591: // EVIOCREVOKE: _IOW('E', 0x91, int)
  case 0x80045705: // WDIOC_KEEPALIVE: _IOR('W', 5, int)
  case 0x40049409: // FICLONE / BTRFS_IOC_CLONE
    return 1;
  default:
    return 0;
  }
}

static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  vfs_node_t *ioctl_node = NULL;
  bool ioctl_handler_called = false;

#if IOCTL_DEBUG_LOGGING
  if (t) {
    klog_puts("[IOCTL] tid=");
    klog_uint64(t->tid);
    klog_puts(" fd=");
    klog_uint64(fd);
    klog_puts(" request=0x");
    klog_hex32(request);
    klog_puts(" arg=0x");
    klog_uint64(arg);
    klog_puts("\n");
  }

  klog_puts("[SYSCALL] sys_ioctl ENTER fd=");
  klog_uint64(fd);
  klog_puts(" request=0x");
  klog_hex32((uint32_t)request);
  klog_puts(" arg=0x");
  klog_hex64(arg);
  klog_puts("\n");
#endif

  if (arg > USER_ADDR_MAX)
    return (uint64_t)-14;

  if (!t || fd >= MAX_FDS)
    return (uint64_t)-9;

  if (fd < MAX_FDS && t->fds[fd]) {
    vfs_node_t *node = t->fds[fd];
    ioctl_node = node;
#if IOCTL_DEBUG_LOGGING
    klog_puts("[SYSCALL] ioctl: node name=");
    klog_puts(node->name);
    klog_puts(" has_ioctl=");
    klog_uint64(node->ioctl ? 1 : 0);
    klog_puts("\n");
#endif

    /* Linux TIOCGPTPEER opens the slave associated with a PTY master.
     * VTE uses this race-free interface and treats ENOTTY as PTY failure. */
    if ((uint32_t)request == 0x5441 && strcmp(node->name, "ptmx") == 0) {
      pty_pair_t *pty = (pty_pair_t *)node->device;
      if (!pty)
        return (uint64_t)-9;
      char peer_path[16] = "/dev/pts/";
      int index = pty->index;
      size_t pos = 9;
      if (index >= 10)
        peer_path[pos++] = (char)(48 + (index / 10));
      peer_path[pos++] = (char)(48 + (index % 10));
      peer_path[pos] = 0;
      return sys_open_path(-100, peer_path, arg, 0);
    }

    if (node->ioctl) {
      ioctl_handler_called = true;
      uint64_t res = (uint64_t)node->ioctl(node, (uint32_t)request, arg);
      if (res != (uint64_t)-25)
        return res;
    }
  }

  uint64_t ret = 0;
  switch ((uint32_t)request) {
  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws || !vmm_is_user_addr_range_valid(arg, sizeof(struct winsize))) {
      ret = (uint64_t)-14;
      break;
    }
    ws->ws_row = (unsigned short)(fb_get_height() / FONT_HEIGHT);
    ws->ws_col = (unsigned short)(fb_get_width() / FONT_WIDTH);
    ws->ws_xpixel = (unsigned short)fb_get_width();
    ws->ws_ypixel = (unsigned short)fb_get_height();
    ret = 0;
    break;
  }
  case TIOCSWINSZ: {
    const struct winsize *ws = (const struct winsize *)arg;
    if (!ws || !vmm_is_user_addr_range_valid(arg, sizeof(struct winsize))) {
      ret = (uint64_t)-14;
      break;
    }
    ret = 0;
    break;
  }
  case TCGETS: {
    if (!arg ||
        !vmm_is_user_addr_range_valid(arg, sizeof(struct kernel_termios))) {
      ret = (uint64_t)-14;
      break;
    }
    struct kernel_termios kt;
    kt.c_iflag = console_termios.c_iflag;
    kt.c_oflag = console_termios.c_oflag;
    kt.c_cflag = console_termios.c_cflag;
    kt.c_lflag = console_termios.c_lflag;
    kt.c_line = console_termios.c_line;
    memcpy(kt.c_cc, console_termios.c_cc, KERNEL_NCCS);
    memcpy((void *)arg, &kt, sizeof(struct kernel_termios));
    ret = 0;
    break;
  }
  case TCGETS2: {
    if (!arg ||
        !vmm_is_user_addr_range_valid(arg, sizeof(struct termios2))) {
      ret = (uint64_t)-14;
      break;
    }
    struct termios2 t2;
    t2.c_iflag = console_termios.c_iflag;
    t2.c_oflag = console_termios.c_oflag;
    t2.c_cflag = console_termios.c_cflag;
    t2.c_lflag = console_termios.c_lflag;
    t2.c_line = console_termios.c_line;
    memcpy(t2.c_cc, console_termios.c_cc, KERNEL_NCCS);
    t2.c_ispeed = 38400;
    t2.c_ospeed = 38400;
    memcpy((void *)arg, &t2, sizeof(struct termios2));
    ret = 0;
    break;
  }
  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    if (!arg ||
        !vmm_is_user_addr_range_valid(arg, sizeof(struct kernel_termios))) {
      ret = (uint64_t)-14;
      break;
    }
    struct kernel_termios kt;
    memcpy(&kt, (const void *)arg, sizeof(struct kernel_termios));
    console_termios.c_iflag = kt.c_iflag;
    console_termios.c_oflag = kt.c_oflag;
    console_termios.c_cflag = kt.c_cflag;
    console_termios.c_lflag = kt.c_lflag;
    console_termios.c_line = kt.c_line;
    memcpy(console_termios.c_cc, kt.c_cc, KERNEL_NCCS);
    ret = 0;
    break;
  }
  case TCSETS2:
  case TCSETSW2:
  case TCSETSF2: {
    if (!arg ||
        !vmm_is_user_addr_range_valid(arg, sizeof(struct termios2))) {
      ret = (uint64_t)-14;
      break;
    }
    struct termios2 t2;
    memcpy(&t2, (const void *)arg, sizeof(struct termios2));
    console_termios.c_iflag = t2.c_iflag;
    console_termios.c_oflag = t2.c_oflag;
    console_termios.c_cflag = t2.c_cflag;
    console_termios.c_lflag = t2.c_lflag;
    console_termios.c_line = t2.c_line;
    memcpy(console_termios.c_cc, t2.c_cc, KERNEL_NCCS);
    ret = 0;
    break;
  }
  case TIOCGPGRP: {
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = (uint64_t)-14;
      break;
    }
    struct thread *curr = sched_get_current();
    int pgrp = curr ? (int)curr->pgid : 1;
    memcpy((void *)arg, &pgrp, sizeof(int));
    ret = 0;
    break;
  }
  case TIOCSPGRP: {
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = (uint64_t)-14;
      break;
    }
    ret = 0;
    break;
  }
  case 0x5470: { // KBDSCANMODE_GET
    int *mode = (int *)arg;
    if (!mode) {
      ret = (uint64_t)-14;
      break;
    }
    extern bool keyboard_is_scancode_mode(void);
    *mode = keyboard_is_scancode_mode() ? 1 : 0;
    ret = 0;
    break;
  }
  case 0x5471: { // KBDSCANMODE_SET
    int mode = (int)arg;
    extern void keyboard_set_scancode_mode(bool enabled);
    keyboard_set_scancode_mode(mode != 0);
    ret = 0;
    break;
  }
  case 0x5472: { // KBDSCANCODE_READ
    unsigned char *event = (unsigned char *)arg;
    if (!event) {
      ret = (uint64_t)-14;
      break;
    }
    extern bool keyboard_has_scancode(void);
    extern bool keyboard_get_scancode(void *event_ptr);
    if (keyboard_has_scancode()) {
      if (keyboard_get_scancode((void *)event)) {
        ret = 1;
        break;
      }
    }
    ret = 0;
    break;
  }
  case VT_OPENQRY: {
    int *vt = (int *)arg;
    if (!vt) {
      ret = (uint64_t)-14;
      break;
    }
    *vt = 1;
    ret = 0;
    break;
  }
  case VT_GETMODE: {
    struct vt_mode *vtm = (struct vt_mode *)arg;
    if (!vtm) {
      ret = (uint64_t)-14;
      break;
    }
    memset(vtm, 0, sizeof(struct vt_mode));
    ret = 0;
    break;
  }
  case VT_SETMODE:
    ret = 0;
    break;
  case VT_GETSTATE: {
    struct vt_stat *vts = (struct vt_stat *)arg;
    if (!vts) {
      ret = (uint64_t)-14;
      break;
    }
    memset(vts, 0, sizeof(struct vt_stat));
    vts->v_active = 1;
    vts->v_state = 0x02;
    ret = 0;
    break;
  }
  case VT_RELDISP:
  case VT_ACTIVATE:
  case VT_WAITACTIVE:
  case VT_DISALLOCATE:
    ret = 0;
    break;
  case KDSETMODE:
  case KDSKBMODE:
    ret = 0;
    break;
  case KDGETMODE: {
    int *mode = (int *)arg;
    if (!mode) {
      ret = (uint64_t)-14;
      break;
    }
    *mode = KD_TEXT;
    ret = 0;
    break;
  }
  case KDGKBMODE: {
    int *mode = (int *)arg;
    if (!mode) {
      ret = (uint64_t)-14;
      break;
    }
    *mode = 0;
    ret = 0;
    break;
  }
  case 0x5451:
    ret = 0;
    break; // KDSIGACCEPT
  case TIOCGETD: {
    int *ldisc = (int *)arg;
    if (!ldisc) {
      ret = (uint64_t)-14;
      break;
    }
    *ldisc = 0;
    ret = 0;
    break;
  }
  case 0x541B: { // FIONREAD / TIOCINQ
    if (!arg || !vmm_is_user_addr_range_valid(arg, sizeof(int))) {
      ret = (uint64_t)-14;
      break;
    }
    int unread = 0;
    if (ioctl_node && (ioctl_node->flags & FS_TYPE_MASK) == FS_FILE) {
      uint64_t off = (fd < MAX_FDS) ? t->fd_offsets[fd] : 0;
      uint64_t len = ioctl_node->length;
      if (len > off) {
        unread = (int)(len - off);
      } else {
        unread = 0;
      }
      *(int *)arg = unread;
      ret = 0;
      break;
    }
    ret = (uint64_t)-25; // ENOTTY
    break;
  }
  case 0x40049409: { // FICLONE / BTRFS_IOC_CLONE
    int src_fd = (int)arg;
    if (src_fd < 0 || src_fd >= MAX_FDS || !t->fds[src_fd]) {
      ret = (uint64_t)-9; // EBADF
      break;
    }
    if (src_fd == (int)fd) {
      ret = (uint64_t)-22; // EINVAL
      break;
    }
    vfs_node_t *dst_node = ioctl_node;
    vfs_node_t *src_node = t->fds[src_fd];
    if (!dst_node || !src_node) {
      ret = (uint64_t)-9; // EBADF
      break;
    }
    if ((dst_node->flags & FS_TYPE_MASK) != FS_FILE ||
        (src_node->flags & FS_TYPE_MASK) != FS_FILE) {
      ret = (uint64_t)-22; // EINVAL: FICLONE requires regular files
      break;
    }
    uint64_t dst_acc = t->fd_flags[fd] & O_ACCMODE;
    if (dst_acc != O_WRONLY && dst_acc != O_RDWR) {
      ret = (uint64_t)-9; // EBADF: destination not open for write
      break;
    }
    vfs_truncate(dst_node, 0);
    uint32_t total = src_node->length;
    if (total == 0) {
      ret = 0;
      break;
    }
    uint8_t *buf = kmalloc(65536);
    if (!buf) {
      ret = (uint64_t)-12; // ENOMEM
      break;
    }
    uint32_t offset = 0;
    bool err = false;
    while (offset < total) {
      uint32_t chunk = (total - offset > 65536) ? 65536 : (total - offset);
      int32_t r = (int32_t)vfs_read(src_node, offset, chunk, buf);
      if (r <= 0) {
        err = true;
        break;
      }
      int32_t w = (int32_t)vfs_write(dst_node, offset, (uint32_t)r, buf);
      if (w != r) {
        err = true;
        break;
      }
      offset += (uint32_t)r;
    }
    kfree(buf);
    if (err) {
      ret = (uint64_t)-5; // EIO
      break;
    }
    vfs_truncate(dst_node, total);
    ret = 0;
    break;
  }
  case 0x4020940D: { // FICLONERANGE
    if (!arg || !vmm_is_user_addr_range_valid(arg, 32)) {
      ret = (uint64_t)-14; // EFAULT
      break;
    }
    struct {
      int64_t src_fd;
      uint64_t src_offset;
      uint64_t src_length;
      uint64_t dest_offset;
    } cr;
    memcpy(&cr, (const void *)arg, sizeof(cr));
    if (cr.src_fd < 0 || cr.src_fd >= MAX_FDS || !t->fds[cr.src_fd]) {
      ret = (uint64_t)-9; // EBADF
      break;
    }
    if (cr.src_fd == (int64_t)fd) {
      ret = (uint64_t)-22; // EINVAL
      break;
    }
    vfs_node_t *dst_node = ioctl_node;
    vfs_node_t *src_node = t->fds[cr.src_fd];
    if (!dst_node || !src_node) {
      ret = (uint64_t)-9;
      break;
    }
    if ((dst_node->flags & FS_TYPE_MASK) != FS_FILE ||
        (src_node->flags & FS_TYPE_MASK) != FS_FILE) {
      ret = (uint64_t)-22;
      break;
    }
    uint64_t dst_acc = t->fd_flags[fd] & O_ACCMODE;
    if (dst_acc != O_WRONLY && dst_acc != O_RDWR) {
      ret = (uint64_t)-9;
      break;
    }
    uint64_t total = cr.src_length;
    if (total == 0) {
      if (cr.src_offset <= src_node->length)
        total = src_node->length - cr.src_offset;
      else
        total = 0;
    }
    if (total == 0) {
      ret = 0;
      break;
    }
    uint8_t *buf = kmalloc(65536);
    if (!buf) {
      ret = (uint64_t)-12;
      break;
    }
    uint64_t copied = 0;
    bool err = false;
    while (copied < total) {
      uint32_t chunk = (total - copied > 65536) ? 65536 : (uint32_t)(total - copied);
      int32_t r = (int32_t)vfs_read(src_node, (uint32_t)(cr.src_offset + copied), chunk, buf);
      if (r <= 0) {
        err = true;
        break;
      }
      int32_t w = (int32_t)vfs_write(dst_node, (uint32_t)(cr.dest_offset + copied), (uint32_t)r, buf);
      if (w != r) {
        err = true;
        break;
      }
      copied += (uint64_t)r;
    }
    kfree(buf);
    if (err) {
      ret = (uint64_t)-5;
      break;
    }
    ret = 0;
    break;
  }
  default:
    ret = (uint64_t)-25; // ENOTTY
    break;
  }

  if (ret == (uint64_t)-25)
    ioctl_log_unhandled(t, fd, (uint32_t)request, arg, ioctl_node,
                        ioctl_handler_called);

#if IOCTL_DEBUG_LOGGING
  klog_puts("[SYSCALL] sys_ioctl RETURN 0x");
  klog_hex64(ret);
  klog_puts("\n");
#endif
  return ret;
}

void syscall_register_ioctl(void) { syscall_register(SYS_IOCTL, sys_ioctl); }
