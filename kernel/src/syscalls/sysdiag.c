// sysdiag.c — Missing Syscalls and Unhandled Ioctls Diagnostic Subsystem
#include "sysdiag.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include "arch/uaccess.h"
#include <stdarg.h>
#include "../apic/lapic_timer.h"
#include "../console/klog.h"
#include "../drivers/serial.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/vma.h"
#include "../sched/sched.h"
#include "../socket/socket.h"
#include <stdbool.h>
#include <stdint.h>

static spinlock_t g_sysdiag_lock = SPINLOCK_INIT;

static sysdiag_syscall_entry_t g_syscall_misses[SYSDIAG_MAX_SYSCALL_MISSES];
static size_t g_syscall_miss_count = 0;
static uint64_t g_syscall_miss_total_calls = 0;

static sysdiag_ioctl_entry_t g_ioctl_misses[SYSDIAG_MAX_IOCTL_MISSES];
static size_t g_ioctl_miss_count = 0;
static uint64_t g_ioctl_miss_total_calls = 0;

static sysdiag_feature_entry_t g_feature_misses[SYSDIAG_MAX_FEATURE_MISSES];
static size_t g_feature_miss_count = 0;
static uint64_t g_feature_miss_total_calls = 0;

static sysdiag_fs_entry_t g_fs_misses[SYSDIAG_MAX_FS_MISSES];
static size_t g_fs_miss_count = 0;
static uint64_t g_fs_miss_total_calls = 0;

static sysdiag_syscall_error_t g_syscall_errors[SYSDIAG_MAX_SYSCALL_ERRORS];
static size_t g_syscall_error_count = 0;
static uint64_t g_syscall_error_total_calls = 0;
static uint64_t g_syscall_error_evictions = 0;

/* Build variable-length diagnostics first, then dispatch each record once so
 * fields from simultaneous syscall paths never interleave on the serial log. */
static void sysdiag_appendf(char *buffer, size_t capacity, size_t *length,
                            const char *fmt, ...) {
  if (!buffer || !length || *length >= capacity)
    return;
  va_list ap;
  va_start(ap, fmt);
  int written = vsnprintf(buffer + *length, capacity - *length, fmt, ap);
  va_end(ap);
  if (written < 0)
    return;
  size_t remaining = capacity - *length;
  if ((size_t)written >= remaining)
    *length = capacity - 1;
  else
    *length += (size_t)written;
}

/* Helper to decode common Linux ioctl request numbers */
static const char *sysdiag_lookup_ioctl_name(uint32_t req) {
  switch (req) {
  /* TTY / Terminal */
  case 0x5401: return "TCGETS";
  case 0x5402: return "TCSETS";
  case 0x5403: return "TCSETSW";
  case 0x5404: return "TCSETSF";
  case 0x540B: return "TCFLSH";
  case 0x540F: return "TIOCGPGRP";
  case 0x5410: return "TIOCSPGRP";
  case 0x5413: return "TIOCGWINSZ";
  case 0x5414: return "TIOCSWINSZ";
  case 0x541B: return "FIONREAD/TIOCINQ";
  case 0x541D: return "TIOCCONS";
  case 0x5421: return "FIONBIO";
  case 0x5422: return "TIOCNOTTY";
  case 0x5420: return "TIOCPKT";
  case 0x80045430: return "TIOCGPTN";
  case 0x40045431: return "TIOCSPTLCK";
  case 0x5430: return "TIOCPKT";
  case 0x5431: return "TIOCGPTN";
  case 0x5432: return "TIOCSPTLCK";
  case 0x80045432: return "TIOCGPTLCK";
  case 0x80045439: return "TIOCGPTLCK";
  case 0x5440: return "TIOCGSID";
  case 0x5441: return "TCGETS2";
  case 0x5442: return "TCSETS2";
  case 0x5450: return "FIONCLEX";
  case 0x5451: return "FIOCLEX";
  case 0x5456: return "TIOCGLCKTRMIOS";
  case 0x5457: return "TIOCSLCKTRMIOS";

  /* Keyboard / Console */
  case 0x4B3A: return "KDSETMODE";
  case 0x4B3B: return "KDGETMODE";
  case 0x4B44: return "KDGKBTYPE";
  case 0x4B45: return "KDSKBENT";
  case 0x4B46: return "KDGKBENT";
  case 0x4B70: return "GIO_CMAP";
  case 0x4B71: return "PIO_CMAP";

  /* Block storage */
  case 0x0301: return "HDIO_GETGEO";
  case 0x1260: return "BLKGETSIZE";
  case 0x1268: return "BLKSSZGET";
  case 0x1272:
  case 0x80081272: return "BLKGETSIZE64";
  case 0x1277: return "BLKDISCARD";

  /* Socket */
  case 0x8910: return "SIOCGIFNAME";
  case 0x8912: return "SIOCGIFCONF";
  case 0x8913: return "SIOCGIFFLAGS";
  case 0x8914: return "SIOCSIFFLAGS";
  case 0x8915: return "SIOCGIFADDR";
  case 0x891B: return "SIOCGIFNETMASK";
  case 0x8927: return "SIOCGIFHWADDR";
  case 0x8933: return "SIOCGIFINDEX";
  case 0x8B07: return "SIOCGIWMODE";

  /* DRM */
  case 0xC0106400: return "DRM_IOCTL_VERSION";
  case 0xC0046402: return "DRM_IOCTL_GET_MAGIC";
  case 0x0000641E: return "DRM_IOCTL_SET_MASTER";
  case 0x0000641F: return "DRM_IOCTL_DROP_MASTER";
  case 0xC04064A0: return "DRM_IOCTL_MODE_GETRESOURCES";
  case 0xC06864A1: return "DRM_IOCTL_MODE_GETCRTC";
  case 0xC06864A2: return "DRM_IOCTL_MODE_SETCRTC";
  case 0xC05064A6: return "DRM_IOCTL_MODE_GETCONNECTOR";
  case 0xC01864A7: return "DRM_IOCTL_MODE_GETPROPERTY";
  case 0xC04064B2: return "DRM_IOCTL_MODE_CREATE_DUMB";
  case 0xC02064B3: return "DRM_IOCTL_MODE_MAP_DUMB";
  case 0xC01064B4: return "DRM_IOCTL_MODE_DESTROY_DUMB";
  case 0xC03864B8: return "DRM_IOCTL_MODE_ATOMIC";
  case 0xC01064C7: return "DRM_IOCTL_MODE_LIST_LESSEES";
  case 0xC00864D0: return "DRM_IOCTL_MODE_CLOSEFB";
  case 0xC0106443: return "DRM_IOCTL_VIRTGPU_GETPARAM";
  case 0xC0186449: return "DRM_IOCTL_VIRTGPU_GET_CAPS";

  /* Evdev */
  case 0x80044501: return "EVIOCGVERSION";
  case 0x80084502: return "EVIOCGID";
  case 0x40044590: return "EVIOCGRAB";
  case 0x40044591: return "EVIOCREVOKE";

  /* File / FS */
  case 0x40049409: return "FICLONE/BTRFS_IOC_CLONE";
  case 0x4020940D: return "FICLONERANGE";
  case 0x81009431: return "BTRFS_IOC_GET_FSLABEL";
  case 0x41009432: return "BTRFS_IOC_SET_FSLABEL";
  default: return NULL;
  }
}

static void sysdiag_decode_ioctl_name(uint32_t req, char *out, size_t outsz) {
  const char *known = sysdiag_lookup_ioctl_name(req);
  if (known) {
    strncpy(out, known, outsz - 1);
    out[outsz - 1] = '\0';
    return;
  }

  /* Linux ioctl encoding: dir:2, size:14, type:8, nr:8 */
  uint8_t type = (req >> 8) & 0xff;
  uint8_t nr = req & 0xff;
  uint32_t size = (req >> 16) & 0x3fff;
  uint32_t dir = (req >> 30) & 0x3;

  const char *dir_str = (dir == 0) ? "IO" : ((dir == 1) ? "IOW" : ((dir == 2) ? "IOR" : "IOWR"));
  char type_char = (type >= 0x20 && type <= 0x7E) ? (char)type : '?';

  /* Format into out */
  char tmp[64];
  size_t p = 0;
  const char *prefix = "_";
  while (*prefix && p < sizeof(tmp) - 1) tmp[p++] = *prefix++;
  const char *d = dir_str;
  while (*d && p < sizeof(tmp) - 1) tmp[p++] = *d++;
  tmp[p++] = '(';
  tmp[p++] = '\'';
  tmp[p++] = type_char;
  tmp[p++] = '\'';
  tmp[p++] = ',';
  tmp[p++] = '0';
  tmp[p++] = 'x';

  /* hex nr */
  const char *hex = "0123456789ABCDEF";
  tmp[p++] = hex[(nr >> 4) & 0xF];
  tmp[p++] = hex[nr & 0xF];
  if (size > 0 && p < sizeof(tmp) - 10) {
    tmp[p++] = ',';
    tmp[p++] = 's';
    tmp[p++] = 'z';
    tmp[p++] = '=';
    char sdigits[8];
    int nd = 0;
    uint32_t s = size;
    do { sdigits[nd++] = (char)('0' + (s % 10)); s /= 10; } while (s && nd < 7);
    while (nd > 0 && p < sizeof(tmp) - 2) tmp[p++] = sdigits[--nd];
  }
  tmp[p++] = ')';
  tmp[p] = '\0';

  strncpy(out, tmp, outsz - 1);
  out[outsz - 1] = '\0';
}

static const char *sysdiag_lookup_errno_name(int64_t err) {
  int e = (int)(err < 0 ? -err : err);
  switch (e) {
  case 1: return "EPERM";
  case 2: return "ENOENT";
  case 3: return "ESRCH";
  case 4: return "EINTR";
  case 5: return "EIO";
  case 6: return "ENXIO";
  case 7: return "E2BIG";
  case 8: return "ENOEXEC";
  case 9: return "EBADF";
  case 10: return "ECHILD";
  case 11: return "EAGAIN";
  case 12: return "ENOMEM";
  case 13: return "EACCES";
  case 14: return "EFAULT";
  case 16: return "EBUSY";
  case 17: return "EEXIST";
  case 18: return "EXDEV";
  case 19: return "ENODEV";
  case 20: return "ENOTDIR";
  case 21: return "EISDIR";
  case 22: return "EINVAL";
  case 23: return "ENFILE";
  case 24: return "EMFILE";
  case 25: return "ENOTTY";
  case 26: return "ETXTBSY";
  case 27: return "EFBIG";
  case 28: return "ENOSPC";
  case 29: return "ESPIPE";
  case 30: return "EROFS";
  case 31: return "EMLINK";
  case 32: return "EPIPE";
  case 33: return "EDOM";
  case 34: return "ERANGE";
  case 36: return "ENAMETOOLONG";
  case 38: return "ENOSYS";
  case 39: return "ENOTEMPTY";
  case 40: return "ELOOP";
  case 88: return "ENOTSOCK";
  case 90: return "EMSGSIZE";
  case 91: return "EPROTOTYPE";
  case 92: return "ENOPROTOOPT";
  case 93: return "EPROTONOSUPPORT";
  case 95: return "EOPNOTSUPP";
  case 97: return "EAFNOSUPPORT";
  case 98: return "EADDRINUSE";
  case 99: return "EADDRNOTAVAIL";
  case 100: return "ENETDOWN";
  case 101: return "ENETUNREACH";
  case 104: return "ECONNRESET";
  case 105: return "ENOBUFS";
  case 106: return "EISCONN";
  case 107: return "ENOTCONN";
  case 110: return "ETIMEDOUT";
  case 111: return "ECONNREFUSED";
  case 115: return "EINPROGRESS";
  default: return NULL;
  }
}

void sysdiag_init(void) {
  spinlock_acquire(&g_sysdiag_lock);
  memset(g_syscall_misses, 0, sizeof(g_syscall_misses));
  g_syscall_miss_count = 0;
  g_syscall_miss_total_calls = 0;

  memset(g_ioctl_misses, 0, sizeof(g_ioctl_misses));
  g_ioctl_miss_count = 0;
  g_ioctl_miss_total_calls = 0;

  memset(g_feature_misses, 0, sizeof(g_feature_misses));
  g_feature_miss_count = 0;
  g_feature_miss_total_calls = 0;

  memset(g_fs_misses, 0, sizeof(g_fs_misses));
  g_fs_miss_count = 0;
  g_fs_miss_total_calls = 0;

  memset(g_syscall_errors, 0, sizeof(g_syscall_errors));
  g_syscall_error_count = 0;
  g_syscall_error_total_calls = 0;
  g_syscall_error_evictions = 0;
  spinlock_release(&g_sysdiag_lock);
}

void sysdiag_record_missing_syscall(uint64_t syscall_num, struct thread *t,
                                    const struct syscall_regs *regs) {
  uint64_t now_ms = lapic_timer_get_ms();
  bool first_time = false;
  const char *name = syscall_get_name(syscall_num);

  spinlock_acquire(&g_sysdiag_lock);
  g_syscall_miss_total_calls++;

  sysdiag_syscall_entry_t *entry = NULL;
  for (size_t i = 0; i < g_syscall_miss_count; i++) {
    if (g_syscall_misses[i].used && g_syscall_misses[i].syscall_num == syscall_num) {
      entry = &g_syscall_misses[i];
      break;
    }
  }

  if (!entry && g_syscall_miss_count < SYSDIAG_MAX_SYSCALL_MISSES) {
    entry = &g_syscall_misses[g_syscall_miss_count++];
    entry->used = true;
    entry->syscall_num = syscall_num;
    strncpy(entry->name, name ? name : "unknown", sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->first_seen_ms = now_ms;
    entry->total_calls = 0;
    first_time = true;
  }

  if (entry) {
    entry->total_calls++;
    entry->last_seen_ms = now_ms;
    if (t) {
      entry->last_tgid = t->tgid;
      entry->last_tid = t->tid;
      strncpy(entry->comm, t->comm[0] ? t->comm : "?", sizeof(entry->comm) - 1);
      entry->comm[sizeof(entry->comm) - 1] = '\0';
    }
    if (regs) {
      entry->last_rip = regs->rip;
      entry->last_args[0] = regs->rdi;
      entry->last_args[1] = regs->rsi;
      entry->last_args[2] = regs->rdx;
      entry->last_args[3] = regs->r10;
      entry->last_args[4] = regs->r8;
      entry->last_args[5] = regs->r9;
    }
  }
  spinlock_release(&g_sysdiag_lock);

  if (first_time) {
    klogf(KLOG_CLR_YELLOW "[SYSDIAG] MISSING SYSCALL: #" KLOG_CLR_RESET
          "%llu (%s) called by comm='%s' pid=%u rip=0x%016llX\n",
          (unsigned long long)syscall_num, name ? name : "unknown",
          (t && t->comm[0]) ? t->comm : "?", t ? t->tgid : 0,
          (unsigned long long)(regs ? regs->rip : 0));
  }
}

void sysdiag_record_missing_ioctl(uint64_t fd, uint32_t request, uint64_t arg,
                                  struct thread *t, struct vfs_node *node,
                                  bool handler_called) {
  /* Filter out known benign probes (e.g. TCFLSH on evdev) */
  if (request == 0x540B && node &&
      (node->flags & FS_TYPE_MASK) == FS_CHARDEV &&
      (strcmp(node->name, "event0") == 0 || strcmp(node->name, "event1") == 0)) {
    return;
  }

  /* F2FS capability probing is expected to fail on regular non-F2FS files. */
  if (request == 0x8004F50C && node &&
      (node->flags & FS_TYPE_MASK) == FS_FILE)
    return;

  /* TIOCGPTN on a non-PTY descriptor: ENOTTY is the correct Linux answer
   * (isatty-style PTY detection by Electron/Chromium on stdio and log
   * files). Only PTY master nodes implement it; anything else reaching
   * here is a benign probe, not an unhandled ioctl. */
  if (request == 0x80045430)
    return;

  /* Wireless-extension probes (e.g. kdeconnectd SIOCGIWMODE on sockets).
   * No wireless hardware exists here; failure is expected. */
  if ((request & 0xFF00) == 0x8B00)
    return;

  uint64_t now_ms = lapic_timer_get_ms();
  bool first_time = false;
  const char *node_name = (node && node->name[0]) ? node->name : "anon";

  spinlock_acquire(&g_sysdiag_lock);
  g_ioctl_miss_total_calls++;

  sysdiag_ioctl_entry_t *entry = NULL;
  for (size_t i = 0; i < g_ioctl_miss_count; i++) {
    if (g_ioctl_misses[i].used && g_ioctl_misses[i].request == request &&
        strcmp(g_ioctl_misses[i].node_name, node_name) == 0) {
      entry = &g_ioctl_misses[i];
      break;
    }
  }

  if (!entry && g_ioctl_miss_count < SYSDIAG_MAX_IOCTL_MISSES) {
    entry = &g_ioctl_misses[g_ioctl_miss_count++];
    entry->used = true;
    entry->request = request;
    sysdiag_decode_ioctl_name(request, entry->name, sizeof(entry->name));
    strncpy(entry->node_name, node_name, sizeof(entry->node_name) - 1);
    entry->node_name[sizeof(entry->node_name) - 1] = '\0';
    entry->node_type = node ? (node->flags & FS_TYPE_MASK) : 0;
    entry->first_seen_ms = now_ms;
    entry->total_calls = 0;
    first_time = true;
  }

  if (entry) {
    entry->total_calls++;
    entry->last_seen_ms = now_ms;
    entry->fd = fd;
    entry->last_arg = arg;
    entry->handler_called = handler_called;
    if (t) {
      entry->last_tgid = t->tgid;
      entry->last_tid = t->tid;
      strncpy(entry->comm, t->comm[0] ? t->comm : "?", sizeof(entry->comm) - 1);
      entry->comm[sizeof(entry->comm) - 1] = '\0';
    }
  }
  spinlock_release(&g_sysdiag_lock);

  if (first_time) {
    char name_buf[48];
    sysdiag_decode_ioctl_name(request, name_buf, sizeof(name_buf));
    klogf(KLOG_CLR_YELLOW "[SYSDIAG] UNHANDLED IOCTL: " KLOG_CLR_RESET
          "0x%08X (%s) node='%s' comm='%s' pid=%u arg=0x%016llX %s\n",
          request, name_buf, node_name,
          (t && t->comm[0]) ? t->comm : "?", t ? t->tgid : 0,
          (unsigned long long)arg,
          handler_called ? "[returned ENOTTY]" : "[no handler]");
  }
}

void sysdiag_record_unimplemented_feature(const char *subsystem,
                                          const char *feature, struct thread *t,
                                          int64_t error_code) {
  uint64_t now_ms = lapic_timer_get_ms();
  bool first_time = false;

  spinlock_acquire(&g_sysdiag_lock);
  g_feature_miss_total_calls++;

  sysdiag_feature_entry_t *entry = NULL;
  for (size_t i = 0; i < g_feature_miss_count; i++) {
    if (g_feature_misses[i].used &&
        strcmp(g_feature_misses[i].subsystem, subsystem ? subsystem : "?") == 0 &&
        strcmp(g_feature_misses[i].feature, feature ? feature : "?") == 0) {
      entry = &g_feature_misses[i];
      break;
    }
  }

  if (!entry && g_feature_miss_count < SYSDIAG_MAX_FEATURE_MISSES) {
    entry = &g_feature_misses[g_feature_miss_count++];
    entry->used = true;
    strncpy(entry->subsystem, subsystem ? subsystem : "?", sizeof(entry->subsystem) - 1);
    entry->subsystem[sizeof(entry->subsystem) - 1] = '\0';
    strncpy(entry->feature, feature ? feature : "?", sizeof(entry->feature) - 1);
    entry->feature[sizeof(entry->feature) - 1] = '\0';
    entry->first_seen_ms = now_ms;
    entry->total_calls = 0;
    first_time = true;
  }

  if (entry) {
    entry->total_calls++;
    entry->last_seen_ms = now_ms;
    entry->return_code = error_code;
    if (t) {
      entry->last_tgid = t->tgid;
      entry->last_tid = t->tid;
      strncpy(entry->comm, t->comm[0] ? t->comm : "?", sizeof(entry->comm) - 1);
      entry->comm[sizeof(entry->comm) - 1] = '\0';
    }
  }
  spinlock_release(&g_sysdiag_lock);

  if (first_time) {
    klogf(KLOG_CLR_YELLOW "[SYSDIAG] UNIMPLEMENTED FEATURE: " KLOG_CLR_RESET
          "%s::%s called by comm='%s' pid=%u ret=%lld\n",
          subsystem ? subsystem : "?", feature ? feature : "?",
          (t && t->comm[0]) ? t->comm : "?", t ? t->tgid : 0,
          (long long)error_code);
  }
}

void sysdiag_record_missing_fs_node(const char *path, struct thread *t,
                                    int64_t error_code, const char *op) {
  if (!path || !path[0]) return;
  // Ignore query to /proc/sysdiag itself
  if (strcmp(path, "/proc/sysdiag") == 0) return;

  uint64_t now_ms = lapic_timer_get_ms();
  bool first_time = false;
  const char *comm = (t && t->comm[0]) ? t->comm : "?";

  spinlock_acquire(&g_sysdiag_lock);
  g_fs_miss_total_calls++;

  sysdiag_fs_entry_t *entry = NULL;
  for (size_t i = 0; i < g_fs_miss_count; i++) {
    if (g_fs_misses[i].used &&
        strcmp(g_fs_misses[i].path, path) == 0 &&
        strcmp(g_fs_misses[i].op, op ? op : "?") == 0) {
      entry = &g_fs_misses[i];
      break;
    }
  }

  if (!entry && g_fs_miss_count < SYSDIAG_MAX_FS_MISSES) {
    entry = &g_fs_misses[g_fs_miss_count++];
    entry->used = true;
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    strncpy(entry->op, op ? op : "?", sizeof(entry->op) - 1);
    entry->op[sizeof(entry->op) - 1] = '\0';
    strncpy(entry->comm, comm, sizeof(entry->comm) - 1);
    entry->comm[sizeof(entry->comm) - 1] = '\0';
    entry->first_seen_ms = now_ms;
    entry->total_calls = 0;
    first_time = true;
  }

  if (entry) {
    entry->total_calls++;
    entry->last_seen_ms = now_ms;
    entry->error_code = error_code;
    if (t) {
      entry->last_tgid = t->tgid;
      entry->last_tid = t->tid;
      strncpy(entry->comm, comm, sizeof(entry->comm) - 1);
      entry->comm[sizeof(entry->comm) - 1] = '\0';
    }
  }
  spinlock_release(&g_sysdiag_lock);

  if (first_time) {
    const char *ename = sysdiag_lookup_errno_name(error_code);
    klogf(KLOG_CLR_YELLOW "[SYSDIAG] MISSING PSEUDO-FS: " KLOG_CLR_RESET
          "op='%s' path='%s' comm='%s' pid=%u ret=%lld%s%s%s\n",
          op ? op : "?", path, comm, t ? t->tgid : 0,
          (long long)error_code, ename ? " (" : "", ename ? ename : "",
          ename ? ")" : "");
  }
}

static bool sysdiag_is_severe_syscall_error(uint64_t num, int64_t err,
                                            const struct syscall_regs *regs) {
  int e = (int)(err < 0 ? -err : err);

  /* Rust's try_statx availability probe deliberately passes NULL for both
   * pointers and expects EFAULT. Ignore r9: statx has only five arguments.
   * Other statx faults remain severe, even from the same process. */
  if (num == 332 && err == -14 && regs && !regs->rdi && !regs->rsi &&
      !regs->rdx && regs->r10 == 0x0fff && !regs->r8)
    return false;

  // prctl(PR_CAPBSET_READ, >40) returning -EINVAL is the standard Linux cap loop end condition
  if (num == 157 /* prctl */ && regs && regs->rdi == 23 /* PR_CAPBSET_READ */ && regs->rsi > 40) {
    return false;
  }

  if (e == 38 /* ENOSYS */ ||
      e == 1  /* EPERM */  ||
      e == 13 /* EACCES */ ||
      e == 14 /* EFAULT */ ||
      e == 25 /* ENOTTY */ ||
      e == 95 /* EOPNOTSUPP */ ||
      e == 97 /* EAFNOSUPPORT */ ||
      e == 92 /* ENOPROTOOPT */) {
    return true;
  }
  if (e == 22 /* EINVAL */) {
    switch (num) {
    case 56:  // clone
    case 435: // clone3
    case 272: // unshare
    case 308: // setns
    case 155: // pivot_root
    case 165: // mount
    case 166: // umount2
    case 157: // prctl
    case 116: // setgroups
    case 126: // capset
    case 321: // bpf
    case 318: // getrandom
    case 319: // memfd_create
    case 247: // waitid
    case 9:   // mmap
    case 10:  // mprotect
    case 202: // futex
      return true;
    default:
      return false;
    }
  }
  if (num == 272 /* unshare */ || num == 308 /* setns */ || num == 155 /* pivot_root */) {
    return true;
  }
  return false;
}

// Copy a user pathname (up to outsz-1 bytes) in fault-safe 16-byte chunks:
// a string running into an unmapped page still yields its valid prefix.
// Always NUL-terminated; "" means no usable bytes.
static void sysdiag_copy_user_path(uint64_t uptr, char *out, size_t outsz) {
  out[0] = '\0';
  if (!uptr || outsz < 2)
    return;
  size_t pos = 0;
  while (pos + 1 < outsz) {
    char chunk[16];
    if (copy_from_user(chunk, (const void *)(uptr + pos), sizeof(chunk)) != 0)
      break;
    for (size_t i = 0; i < sizeof(chunk) && pos + 1 < outsz; i++) {
      out[pos++] = chunk[i];
      if (chunk[i] == '\0')
        return;
    }
  }
  out[pos] = '\0';
}

// Best-effort pathname for a failing syscall, for the SYSDIAG report.
// open/stat-style syscalls take the path in rdi, *at-style in rsi;
// connect contributes AF_UNIX socket paths ("@name" for abstract).
// "" when the syscall takes no path.
static void sysdiag_capture_error_path(uint64_t num,
                                        const struct syscall_regs *regs,
                                        char *out, size_t outsz) {
  out[0] = '\0';
  if (!regs)
    return;
  uint64_t path_ptr = 0;
  switch (num) {
  case 9: { // file-backed mmap: pathname comes from its descriptor, not addr
    if (regs->r10 & 0x20 /* MAP_ANONYMOUS */)
      return;
    struct thread *t = sched_get_current();
    uint32_t fd = (uint32_t)regs->r8; /* mmap consumes only the low int. */
    if (t && fd < MAX_FDS)
      sched_get_fd_path_snapshot(t->tid, fd, out, outsz);
    return;
  }
  case 2:  // open
  case 4:  // stat
  case 6:  // lstat
  case 21: // access
  case 59: // execve
  case 83: // mkdir
  case 84: // rmdir
  case 87: // unlink
  case 89: // readlink
  case 188: // setxattr
  case 189: // lsetxattr
  case 191: // getxattr
  case 192: // lgetxattr
  case 194: // listxattr
  case 195: // llistxattr
  case 197: // removexattr
  case 198: // lremovexattr
    path_ptr = regs->rdi;
    break;
  case 82: { // rename: include both names so filesystem limitations are actionable
    char old_path[96], new_path[96];
    sysdiag_copy_user_path(regs->rdi, old_path, sizeof(old_path));
    sysdiag_copy_user_path(regs->rsi, new_path, sizeof(new_path));
    if (outsz > 1)
      snprintf(out, outsz, "%s -> %s", old_path, new_path);
    return;
  }
  case 257: // openat
  case 262: // newfstatat
  case 269: // faccessat
  case 439: // faccessat2
  case 267: // readlinkat
  case 332: // statx
  case 258: // mkdirat
  case 263: // unlinkat
    path_ptr = regs->rsi;
    break;
  case 42: { // connect: AF_UNIX pathname from the sockaddr
    if (!regs->rsi || regs->rdx < 3)
      return;
    uint8_t sa[128];
    uint64_t len = regs->rdx > sizeof(sa) ? sizeof(sa) : regs->rdx;
    if (copy_from_user(sa, (const void *)regs->rsi, (size_t)len) != 0)
      return;
    if (sa[0] != 1 || sa[1] != 0) // not AF_UNIX (LE)
      return;
    const char *sp = (const char *)&sa[2];
    size_t maxn = (size_t)len - 2;
    if (sp[0] == '\0') { // abstract namespace
      if (maxn < 2)
        return;
      size_t n = 0;
      out[0] = '@';
      while (n + 1 < maxn && n + 2 < outsz && sp[1 + n] != '\0') {
        out[1 + n] = sp[1 + n];
        n++;
      }
      out[1 + n] = '\0';
    } else {
      size_t n = 0;
      while (n < maxn && n + 1 < outsz && sp[n] != '\0') {
        out[n] = sp[n];
        n++;
      }
      out[n] = '\0';
    }
    return;
  }
  default:
    return;
  }
  sysdiag_copy_user_path(path_ptr, out, outsz);
}

/* Copy the attribute name before taking the diagnostic lock. Never classify
 * a faulted or truncated name as a known probe. listxattr has no name arg. */
static bool sysdiag_capture_xattr_name(uint64_t num,
                                      const struct syscall_regs *regs,
                                      char *out, size_t outsz) {
  out[0] = '\0';
  if (!regs || !regs->rsi || outsz < 2 ||
      !((num >= 188 && num <= 193) || (num >= 197 && num <= 199)))
    return false;
  long n = strncpy_from_user(out, (const char *)regs->rsi, (long)outsz);
  if (n < 0 || (size_t)n >= outsz) {
    out[0] = '\0';
    return false;
  }
  return true;
}

static bool sysdiag_is_acl_capability_probe(uint64_t num, int64_t err,
                                           const struct syscall_regs *regs,
                                           const char *name) {
  /* KIO probes ACLs both with a size query and with an allocated buffer.
   * Unsupported ACL reads are expected until storage/enforcement exists.
   * Keep writes and reads of other namespaces visible; do not advertise
   * ACL support by changing EOPNOTSUPP into ENODATA or fabricated data. */
  return err == -95 && num >= 191 && num <= 193 && regs &&
         (strcmp(name, "system.posix_acl_access") == 0 ||
          strcmp(name, "system.posix_acl_default") == 0);
}

static bool sysdiag_is_sddm_theme_write_probe(uint64_t num, int64_t err,
                                             const struct thread *t,
                                             const struct syscall_regs *regs) {
  /* Qt queries writability while reading icon-theme metadata. Restrict this
   * exception to the observed greeter, exact theme and plain W_OK query;
   * do not hide real open/write failures or unrelated permission denials. */
  if (num != 21 || err != -13 || !t || !regs || regs->rsi != 2 ||
      strcmp(t->comm, "sddm-greeter-qt") != 0)
    return false;
  char path[96];
  long len = strncpy_from_user(path, (const char *)regs->rdi, sizeof(path));
  if (len < 0 || (size_t)len >= sizeof(path)) return false;
  return strcmp(path, "/usr/share/icons/breeze/index.theme") == 0;
}

static bool sysdiag_is_firefox_userns_probe(uint64_t num, int64_t err,
                                           const struct thread *t,
                                           const struct syscall_regs *regs) {
  /* Mozilla's CanCreateUserNamespace forks only to test kernel support.
   * Keep sandbox launches and other clone failures severe. */
  return num == 56 && err == -1 && t && regs &&
         strcmp(t->comm, "firefox") == 0 &&
         regs->rdi == (CLONE_NEWUSER | 17 /* SIGCHLD */) &&
         !regs->rsi && !regs->rdx && !regs->r10 && !regs->r8;
}

static bool sysdiag_is_readonly_rlimit_probe(uint64_t num, int64_t err,
                                            struct thread *t,
                                            const struct syscall_regs *regs) {
  /* Chromium CheckMemoryReadOnly() calls getrlimit(RLIMIT_NPROC) on the
   * start of a protected page and requires EFAULT. glibc uses prlimit64.
   * Require mapping evidence as well as the ABI shape: a NULL/unmapped
   * buffer or a failure copying to writable memory remains a real error. */
  if (num != 302 || err != -14 || !t || !t->mm || !regs ||
      (uint32_t)regs->rdi != 0 || (uint32_t)regs->rsi != 6 || regs->rdx ||
      !regs->r10 || (regs->r10 & 4095) || regs->r10 > USER_SPACE_LIMIT - 15)
    return false;

  spinlock_acquire(&t->mm->lock);
  struct vma *v = vma_find(&t->mm->vmas, regs->r10);
  bool readonly = v && (v->prot & 1 /* PROT_READ */) &&
                  !(v->prot & 2 /* PROT_WRITE */) &&
                  v->end - regs->r10 >= 16 /* struct rlimit64 */;
  spinlock_release(&t->mm->lock);
  return readonly;
}

/* Read/write-family syscalls whose EAGAIN names a descriptor that also has a
 * poll() event.  Return the fd and the readiness bit the call consumed. */
static bool sysdiag_eagain_fd_event(uint64_t num,
                                    const struct syscall_regs *regs,
                                    uint64_t *fd_out, int *events_out) {
  if (!regs || !fd_out || !events_out)
    return false;
  switch (num) {
  case 0:  /* read */
  case 17: /* pread64 */
  case 45: /* recvfrom */
  case 47: /* recvmsg */
    *events_out = POLLIN;
    break;
  case 1:  /* write */
  case 18: /* pwrite64 */
  case 44: /* sendto */
  case 46: /* sendmsg */
    *events_out = POLLOUT;
    break;
  default:
    return false;
  }
  *fd_out = (uint32_t)regs->rdi;
  return true;
}

void sysdiag_record_syscall_error(uint64_t syscall_num, int64_t error_code,
                                  struct thread *t,
                                  const struct syscall_regs *regs) {
  if (error_code >= 0) return;

  uint64_t now_ms = lapic_timer_get_ms();
  bool first_time = false;
  bool severe = sysdiag_is_severe_syscall_error(syscall_num, error_code, regs);
  /* The access handler supplies resolved inode/type evidence. Keep normal
   * negative execute probes counted, but avoid alarming live warnings.
   * Check the ABI mode too: aggregation may mix probes and real denials. */
  bool nonexec_probe = t && t->last_access_nonexec_probe;
  if (t) t->last_access_nonexec_probe = false;
  /* A seccomp RET_ERRNO decision answers the call without running its
   * handler, so the recorded errno belongs to the application's own filter,
   * not to any kernel permission check. Consume the per-thread marker here
   * and carry the attribution through the entry and every report line. */
  bool via_seccomp = t &&
      __atomic_exchange_n(&t->seccomp_deny_pending, false, __ATOMIC_ACQ_REL);
  if (via_seccomp)
    severe = false;
  if (nonexec_probe && error_code == -13 && regs &&
      ((syscall_num == 21 && regs->rsi == 1) ||
       ((syscall_num == 269 || syscall_num == 439) && regs->rdx == 1)))
    severe = false;
  if (sysdiag_is_sddm_theme_write_probe(syscall_num, error_code, t, regs))
    severe = false;
  if (sysdiag_is_firefox_userns_probe(syscall_num, error_code, t, regs))
    severe = false;
  if (sysdiag_is_readonly_rlimit_probe(syscall_num, error_code, t, regs))
    severe = false;
  const char *comm = (t && t->comm[0]) ? t->comm : "?";
  char cap_attr[256];
  if (sysdiag_capture_xattr_name(syscall_num, regs, cap_attr, sizeof(cap_attr)) &&
      sysdiag_is_acl_capability_probe(syscall_num, error_code, regs, cap_attr))
    severe = false;

  /* Known-benign probes that Linux also answers with an error: never severe.
   * - getsockopt(SOL_SOCKET, SO_PEERSEC=31) with no LSM (dbus-daemon peer
   *   labeling; fails ENOPROTOOPT on Linux too).
   * - TCFLSH on evdev input nodes (Xorg terminal probe; the evdev handler
   *   intentionally returns ENOTTY and sysdiag_record_missing_ioctl already
   *   filters it — this quiets the generic error line as well). */
  if (severe && regs && t) {
    if (syscall_num == 444 /* landlock_create_ruleset */ && error_code == -38 &&
        !regs->rdi && !regs->rsi && (uint32_t)regs->rdx == 1) {
      /* Explicit unsupported ABI-version query, not failed enforcement. */
      severe = false;
    } else if (syscall_num == 55 && error_code == -92 &&
               (int)regs->rsi == 6 && (int)regs->rdx == 11 /* TCP_INFO */) {
      /* Optional TCP statistics are not implemented by our network stack. */
      severe = false;
    } else if (syscall_num == 16 && error_code == -25 &&
               (uint32_t)regs->rsi == 0x8004F50C) {
      vfs_node_t *probe = fd_node_get(t, (uint32_t)regs->rdi);
      if (probe && (probe->flags & FS_TYPE_MASK) == FS_FILE)
        severe = false; /* F2FS_IOC_GET_FEATURES on a non-F2FS file. */
      fd_node_put(&probe);
    } else if (syscall_num == 55 /* getsockopt */ && error_code == -92 &&
        regs->rsi == 1 /* SOL_SOCKET */ &&
        (regs->rdx == 31 /* SO_PEERSEC */ || regs->rdx == 59 /* SO_PEERGROUPS */ ||
         regs->rdx == 77 /* SO_PEERPIDFD */)) {
      severe = false;
    } else if (syscall_num == 54 /* setsockopt */ && error_code == -92 &&
               regs->rsi == 6 /* IPPROTO_TCP / SOL_TCP */ &&
               regs->rdx == 1 /* TCP_NODELAY */) {
      severe = false;
    } else if ((syscall_num == 303 /* name_to_handle_at */ ||
                syscall_num == 304 /* open_by_handle_at */) &&
               error_code == -95 /* EOPNOTSUPP */) {
      severe = false;
    } else if (syscall_num == 227 /* clock_settime */ && error_code == -1 /* EPERM */) {
      /* No RTC write path in AvoryOS. */
      severe = false;
    } else if (syscall_num == 16 /* ioctl */ && error_code == -25 &&
               (uint32_t)regs->rsi == 0x540B /* TCFLSH */ &&
               FD_IN_RANGE(t, regs->rdi)) {
      vfs_node_t *node = t->fds[regs->rdi];
      if (fd_is_valid_node(node) && (node->flags & FS_TYPE_MASK) == FS_CHARDEV &&
          (strcmp(node->name, "event0") == 0 ||
           strcmp(node->name, "event1") == 0))
        severe = false;
    } else if (error_code == -95 /* EOPNOTSUPP */) {
      /* O_TMPFILE create probe (Qt QSaveFile/QQmlThread writing qmlcache):
       * sys_open answers EOPNOTSUPP like a pre-3.11 kernel and Qt falls back
       * to a named temporary file, which it fully implements. flags carry
       * O_TMPFILE (__O_TMPFILE|O_DIRECTORY = 0x410000) with write access:
       * rsi for open, rdx for openat. */
      uint64_t oflags = 0;
      if (syscall_num == 2 /* open */)
        oflags = regs->rsi;
      else if (syscall_num == 257 /* openat */)
        oflags = regs->rdx;
      if ((oflags & 0x410000) == 0x410000 && (oflags & 3) != 0)
        severe = false;
    } else if (syscall_num == 112 /* setsid */ && error_code == -1) {
      /* POSIX mandates EPERM when the caller is already a process group
       * leader; PID 1 (pgid 1 by design) can never setsid. Busybox init
       * calls it unconditionally and ignores the failure — same on Linux. */
      severe = false;
    } else if (syscall_num == 202 /* futex */ && error_code == -1 &&
               (regs->rsi & 0x7f) == 7 /* FUTEX_UNLOCK_PI */) {
      /* Non-owner PI unlock (fork/robust handoff at thread startup):
       * pipewire, pipewire-pulse and wireplumber each trip this once while
       * initializing their RT threads and continue normally — Linux answers
       * the same EPERM and the callers handle it. */
      severe = false;
    } else if (error_code == -38 /* ENOSYS */ &&
               (syscall_num == 428 /* open_tree */ ||
                syscall_num == 429 /* move_mount */ ||
                syscall_num == 430 /* fsopen */ ||
                syscall_num == 431 /* fsconfig */ ||
                syscall_num == 432 /* fsmount */ ||
                syscall_num == 433 /* fspick */ ||
                syscall_num == 442 /* mount_setattr */ ||
                syscall_num == 323 /* userfaultfd */ ||
                syscall_num == 444 /* landlock_create_ruleset */ ||
                syscall_num == 445 /* landlock_add_rule */ ||
                syscall_num == 446 /* landlock_restrict_self */)) {
      /* Deliberate stubs (no fs-context, userfaultfd, or landlock yet). */
      severe = false;
    } else if (error_code == -38 /* ENOSYS */ &&
               (syscall_num == 425 /* io_uring_setup */ ||
                syscall_num == 426 /* io_uring_enter */ ||
                syscall_num == 427 /* io_uring_register */)) {
      /* io_uring setup rejects feature requests the sync executor does
       * not provide (SQPOLL/IOPOLL and friends); callers fall back,
       * same as a Linux kernel without that configuration. */
      severe = false;
    } else if (syscall_num == 16 /* ioctl */ && error_code == -25 &&
               (uint32_t)regs->rsi == 0x80045430 /* TIOCGPTN */) {
      /* PTY-number probe on a non-PTY descriptor (Electron stdio/log
       * isatty check). Linux also answers ENOTTY here; only a PTY
       * master implements TIOCGPTN. */
      severe = false;
    } else if (syscall_num == 16 /* ioctl */ && error_code == -25 &&
               (((uint32_t)regs->rsi & 0xFF00) == 0x8B00)) {
      /* Wireless-extension probe (kdeconnectd SIOCGIWMODE): no wireless
       * hardware exists, so ENOTTY is the expected answer. */
      severe = false;
    } else if (syscall_num == 54 /* setsockopt */ && error_code == -92 &&
               regs->rsi == 1 /* SOL_SOCKET */ &&
               (regs->rdx == 2 /* SO_REUSEADDR */ ||
                regs->rdx == 15 /* SO_REUSEPORT */)) {
      /* Unsupported on AF_UNIX, same as Linux; GLib/GVFS probes these
       * unconditionally and handles ENOPROTOOPT. */
      severe = false;
    }
  }

  // Phase 1: is this (num,err,comm) key already tracked? Path capture uses
  // copy_from_user, which must not run under the sysdiag spinlock.
  bool key_known = false;
  spinlock_acquire(&g_sysdiag_lock);
  for (size_t i = 0; i < g_syscall_error_count; i++) {
    if (g_syscall_errors[i].used &&
        g_syscall_errors[i].syscall_num == syscall_num &&
        g_syscall_errors[i].error_code == error_code &&
        strcmp(g_syscall_errors[i].comm, comm) == 0) {
      key_known = !severe || g_syscall_errors[i].is_severe;
      break;
    }
  }
  spinlock_release(&g_sysdiag_lock);

  /* EAGAIN from a descriptor-bearing read/write call: record poll()'s view of
   * the same fd.  A readiness source that keeps reporting POLLIN/POLLOUT
   * while the matching syscall keeps returning EAGAIN is exactly the stale
   * readiness loop that burns a CPU in an event thread.  Sample at most once
   * per second per (syscall,errno,comm) key so a hot loop stays cheap. */
  char cap_detail[160] = {0};
  bool detail_sampled = false;
  bool poll_claims_ready = false;
  uint64_t detail_fd = 0;
  int detail_events = 0;
  if (error_code == -11 && t &&
      sysdiag_eagain_fd_event(syscall_num, regs, &detail_fd, &detail_events)) {
    bool due = true;
    spinlock_acquire(&g_sysdiag_lock);
    for (size_t i = 0; i < g_syscall_error_count; i++) {
      if (g_syscall_errors[i].used &&
          g_syscall_errors[i].syscall_num == syscall_num &&
          g_syscall_errors[i].error_code == error_code &&
          strcmp(g_syscall_errors[i].comm, comm) == 0) {
        if (now_ms - g_syscall_errors[i].last_detail_ms < 1000)
          due = false;
        break;
      }
    }
    spinlock_release(&g_sysdiag_lock);

    if (due) {
      detail_sampled = true;
      vfs_node_t *node = fd_node_get(t, detail_fd);
      if (node) {
        if ((node->flags & FS_TYPE_MASK) == FS_SOCKET && node->device) {
          int revents = vfs_poll(node, detail_events) &
                        (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL);
          socket_t *sock = (socket_t *)node->device;
          char desc[120] = {0};
          socket_diag_describe(sock, desc, sizeof(desc));
          if (desc[0]) {
            snprintf(cap_detail, sizeof(cap_detail),
                     "fd=%llu ev=0x%x r=0x%x %s",
                     (unsigned long long)detail_fd, detail_events, revents,
                     desc);
            poll_claims_ready = (revents & detail_events) != 0;
          }
        }
        fd_node_put(&node);
      }
    }
  }

  // Best-effort pathname, captured at most once per unique key.
  char cap_path[96] = {0};
  if (!key_known && regs)
    sysdiag_capture_error_path(syscall_num, regs, cap_path, sizeof(cap_path));

  spinlock_acquire(&g_sysdiag_lock);
  g_syscall_error_total_calls++;

  sysdiag_syscall_error_t *entry = NULL;
  for (size_t i = 0; i < g_syscall_error_count; i++) {
    if (g_syscall_errors[i].used &&
        g_syscall_errors[i].syscall_num == syscall_num &&
        g_syscall_errors[i].error_code == error_code &&
        strcmp(g_syscall_errors[i].comm, comm) == 0) {
      entry = &g_syscall_errors[i];
      break;
    }
  }

  if (!entry) {
    if (g_syscall_error_count < SYSDIAG_MAX_SYSCALL_ERRORS) {
      entry = &g_syscall_errors[g_syscall_error_count++];
    } else {
      /* Startup probes must not permanently hide errors from applications
       * launched later. Retain recently observed keys in the bounded table. */
      entry = &g_syscall_errors[0];
      for (size_t i = 1; i < g_syscall_error_count; i++) {
        if (g_syscall_errors[i].last_seen_ms < entry->last_seen_ms)
          entry = &g_syscall_errors[i];
      }
      g_syscall_error_evictions++;
    }
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->syscall_num = syscall_num;
    entry->error_code = error_code;
    const char *sname = syscall_get_name(syscall_num);
    strncpy(entry->name, sname ? sname : "unknown", sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    strncpy(entry->comm, comm, sizeof(entry->comm) - 1);
    entry->comm[sizeof(entry->comm) - 1] = '\0';
    if (cap_path[0]) {
      strncpy(entry->path, cap_path, sizeof(entry->path) - 1);
      entry->path[sizeof(entry->path) - 1] = '\0';
    }
    entry->is_severe = severe;
    if (via_seccomp)
      entry->via_seccomp = true;
    entry->first_seen_ms = now_ms;
    entry->total_calls = 0;
    first_time = true;
  }

  if (entry) {
    /* Names are not part of the aggregation key. An expected ACL probe must
     * not hide a later real xattr failure from the same worker. */
    if (severe && !entry->is_severe) {
      entry->is_severe = true;
      first_time = true;
      if (cap_path[0]) {
        strncpy(entry->path, cap_path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
      }
    }
    if (via_seccomp)
      entry->via_seccomp = true;
    entry->total_calls++;
    entry->last_seen_ms = now_ms;
    if (t) {
      entry->last_tgid = t->tgid;
      entry->last_tid = t->tid;
    }
    if (regs) {
      entry->last_rip = regs->rip;
      entry->last_args[0] = regs->rdi;
      entry->last_args[1] = regs->rsi;
      entry->last_args[2] = regs->rdx;
      entry->last_args[3] = regs->r10;
      entry->last_args[4] = regs->r8;
      entry->last_args[5] = regs->r9;
    }
    if (detail_sampled) {
      entry->last_detail_ms = now_ms;
      if (cap_detail[0]) {
        strncpy(entry->detail, cap_detail, sizeof(entry->detail) - 1);
        entry->detail[sizeof(entry->detail) - 1] = '\0';
      }
    }
  }
  spinlock_release(&g_sysdiag_lock);

  if (first_time && severe) {
    const char *sname = syscall_get_name(syscall_num);
    const char *ename = sysdiag_lookup_errno_name(error_code);
    bool unsupported_namespace = syscall_num == 56 && error_code == -1 &&
        regs && (regs->rdi & (CLONE_NEWCGROUP | CLONE_NEWUSER | CLONE_NEWTIME));
    bool unsupported_strict_check = syscall_num == 54 && error_code == -92 &&
        regs && regs->rsi == 270 && regs->rdx == 12;
    char line[1536];
    size_t line_len = 0;
    sysdiag_appendf(line, sizeof(line), &line_len, "%s%s(#%llu) returned -%llu",
        unsupported_namespace
            ? KLOG_CLR_YELLOW "[SYSDIAG] UNSUPPORTED NAMESPACES: " KLOG_CLR_RESET
            : KLOG_CLR_YELLOW "[SYSDIAG] SYSCALL ERROR: " KLOG_CLR_RESET,
        sname ? sname : "syscall", (unsigned long long)syscall_num,
        (unsigned long long)-error_code);
    if (ename)
      sysdiag_appendf(line, sizeof(line), &line_len, " (%s)", ename);
    if (via_seccomp)
      sysdiag_appendf(line, sizeof(line), &line_len,
          " via seccomp deny (the handler never ran; errno came from the caller's own filter)");
    sysdiag_appendf(line, sizeof(line), &line_len, " called by comm='%s' pid=%u",
                    comm, t ? t->tgid : 0);
    if (regs) {
      sysdiag_appendf(line, sizeof(line), &line_len,
          " args=(0x%016llX, 0x%016llX, 0x%016llX, 0x%016llX, 0x%016llX, 0x%016llX)",
          (unsigned long long)regs->rdi, (unsigned long long)regs->rsi,
          (unsigned long long)regs->rdx, (unsigned long long)regs->r10,
          (unsigned long long)regs->r8, (unsigned long long)regs->r9);
    }
    sysdiag_appendf(line, sizeof(line), &line_len, " rip=0x%016llX",
                    (unsigned long long)(regs ? regs->rip : 0));
    if (cap_path[0])
      sysdiag_appendf(line, sizeof(line), &line_len, " path='%s'", cap_path);
    if (cap_attr[0])
      sysdiag_appendf(line, sizeof(line), &line_len, " xattr='%s'", cap_attr);
    if (unsupported_namespace)
      sysdiag_appendf(line, sizeof(line), &line_len,
                      " reason='requested namespace isolation is not implemented'");
    if (unsupported_strict_check)
      sysdiag_appendf(line, sizeof(line), &line_len,
          " option='NETLINK_GET_STRICT_CHK' reason='strict Netlink request validation is not implemented'");
    if (syscall_num == 9 && error_code == -22 && regs && !regs->rsi)
      sysdiag_appendf(line, sizeof(line), &line_len,
                      " reason='zero-length mapping requested'");
    sysdiag_appendf(line, sizeof(line), &line_len, "\n");
    klogf("%s", line);
  }
}

/* Safe serial-direct output helper for emergency dump (just like lockdiag) */
static void sd_put(const char *s) {
  if (s) {
    serial_write_sync(s, strlen(s));
  }
}

static void sd_dec(uint64_t v) {
  char buf[24];
  int i = (int)sizeof(buf);
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v != 0 && i > 0);
  serial_write_sync(&buf[i], (size_t)(sizeof(buf) - (size_t)i));
}

static void sd_hex32(uint32_t v) {
  char buf[10];
  const char *hex = "0123456789ABCDEF";
  for (int i = 0; i < 8; i++) {
    buf[i] = hex[(v >> ((7 - i) * 4)) & 0xF];
  }
  serial_write_sync(buf, 8);
}

static void sd_hex64(uint64_t v) {
  char buf[18];
  buf[0] = '0';
  buf[1] = 'x';
  const char *hex = "0123456789ABCDEF";
  for (int i = 0; i < 16; i++) {
    buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
  }
  serial_write_sync(buf, 18);
}

/* Game-client comms run under Mocktail/Wine with generic worker names; match
 * the distinctive launcher/runtime substrings case-insensitively. */
static bool sysdiag_comm_is_game(const char *comm) {
  if (!comm || !comm[0])
    return false;
  char low[32];
  unsigned n = 0;
  while (n < 31 && comm[n]) {
    char c = comm[n];
    low[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    n++;
  }
  low[n] = '\0';
  const char *tags[] = {"mocktail", "roblox", "wine", "sober"};
  for (unsigned k = 0; k < 4; k++) {
    const char *tag = tags[k];
    unsigned tl = 0;
    while (tag[tl]) tl++;
    if (!tl || tl > n)
      continue;
    for (unsigned i = 0; i + tl <= n; i++) {
      unsigned j = 0;
      while (j < tl && low[i + j] == tag[j]) j++;
      if (j == tl)
        return true;
    }
  }
  return false;
}

/* Join-stall error set from the audit trail: extent-insert EIO, ENOSPC,
 * cross-device rename EXDEV, peer timeout, broken pipe. EAGAIN/EINTR and
 * futex timeouts are ordinary event-loop results and stay out. */
static bool sysdiag_err_is_join_signature(const sysdiag_syscall_error_t *e) {
  if (!e || !e->used)
    return false;
  int err = (int)-e->error_code;
  /* Apply exclusions before the game-name shortcut. A timed condition wait
   * expiring or a nonblocking receive finding no data is normal progress,
   * even in a game worker. Network ETIMEDOUT remains a useful candidate. */
  if (err == 11 || err == 4 || (e->syscall_num == 202 && err == 110))
    return false;
  if (sysdiag_comm_is_game(e->comm))
    return true;
  return err == 5 || err == 28 || err == 18 || err == 110 || err == 32;
}

void sysdiag_dump_report(void) {
  sd_put("\n[SYSDIAG] ================= UNHANDLED / MISSING COMPAT =================\n");

  spinlock_acquire(&g_sysdiag_lock);

  /* Missing syscalls */
  sd_put("[SYSDIAG] missing syscalls (unique: ");
  sd_dec(g_syscall_miss_count);
  sd_put(", total invocations: ");
  sd_dec(g_syscall_miss_total_calls);
  sd_put("):\n");

  if (g_syscall_miss_count == 0) {
    sd_put("[SYSDIAG]   (none)\n");
  } else {
    for (size_t i = 0; i < g_syscall_miss_count; i++) {
      sysdiag_syscall_entry_t *e = &g_syscall_misses[i];
      sd_put("[SYSDIAG]   #");
      sd_dec(i + 1);
      sd_put(": syscall ");
      sd_dec(e->syscall_num);
      sd_put(" (");
      sd_put(e->name);
      sd_put(") called ");
      sd_dec(e->total_calls);
      sd_put("x by comm='");
      sd_put(e->comm[0] ? e->comm : "?");
      sd_put("' pid=");
      sd_dec(e->last_tgid);
      sd_put(" rip=");
      sd_hex64(e->last_rip);
      sd_put(" args=(");
      sd_hex64(e->last_args[0]);
      sd_put(", ");
      sd_hex64(e->last_args[1]);
      sd_put(", ");
      sd_hex64(e->last_args[2]);
      sd_put(")\n");
    }
  }

  /* Unhandled ioctls */
  sd_put("[SYSDIAG] unhandled ioctls (unique: ");
  sd_dec(g_ioctl_miss_count);
  sd_put(", total invocations: ");
  sd_dec(g_ioctl_miss_total_calls);
  sd_put("):\n");

  if (g_ioctl_miss_count == 0) {
    sd_put("[SYSDIAG]   (none)\n");
  } else {
    for (size_t i = 0; i < g_ioctl_miss_count; i++) {
      sysdiag_ioctl_entry_t *e = &g_ioctl_misses[i];
      sd_put("[SYSDIAG]   #");
      sd_dec(i + 1);
      sd_put(": ioctl 0x");
      sd_hex32(e->request);
      sd_put(" (");
      sd_put(e->name);
      sd_put(") on node='");
      sd_put(e->node_name[0] ? e->node_name : "anon");
      sd_put("' called ");
      sd_dec(e->total_calls);
      sd_put("x by comm='");
      sd_put(e->comm[0] ? e->comm : "?");
      sd_put("' pid=");
      sd_dec(e->last_tgid);
      sd_put(" fd=");
      sd_dec(e->fd);
      sd_put(" last_arg=");
      sd_hex64(e->last_arg);
      sd_put(e->handler_called ? " [-ENOTTY]\n" : " [no-handler]\n");
    }
  }

  /* Unimplemented features */
  if (g_feature_miss_count > 0) {
    sd_put("[SYSDIAG] unimplemented features / flags (unique: ");
    sd_dec(g_feature_miss_count);
    sd_put("):\n");
    for (size_t i = 0; i < g_feature_miss_count; i++) {
      sysdiag_feature_entry_t *e = &g_feature_misses[i];
      sd_put("[SYSDIAG]   #");
      sd_dec(i + 1);
      sd_put(": ");
      sd_put(e->subsystem);
      sd_put("::");
      sd_put(e->feature);
      sd_put(" called ");
      sd_dec(e->total_calls);
      sd_put("x by comm='");
      sd_put(e->comm[0] ? e->comm : "?");
      sd_put("' pid=");
      sd_dec(e->last_tgid);
      sd_put(" ret=");
      if (e->return_code < 0) {
        sd_put("-");
        sd_dec((uint64_t)-e->return_code);
      } else {
        sd_dec((uint64_t)e->return_code);
      }
      sd_put("\n");
    }
  }

  /* Missing /proc & /sys pseudo-fs nodes */
  if (g_fs_miss_count > 0) {
    sd_put("[SYSDIAG] missing /proc & /sys nodes (unique: ");
    sd_dec(g_fs_miss_count);
    sd_put(", total hits: ");
    sd_dec(g_fs_miss_total_calls);
    sd_put("):\n");
    for (size_t i = 0; i < g_fs_miss_count; i++) {
      sysdiag_fs_entry_t *e = &g_fs_misses[i];
      sd_put("[SYSDIAG]   #");
      sd_dec(i + 1);
      sd_put(": [");
      sd_put(e->op);
      sd_put("] '");
      sd_put(e->path);
      sd_put("' hits=");
      sd_dec(e->total_calls);
      sd_put(" comm='");
      sd_put(e->comm[0] ? e->comm : "?");
      sd_put("' pid=");
      sd_dec(e->last_tgid);
      sd_put(" ret=");
      if (e->error_code < 0) {
        sd_put("-");
        sd_dec((uint64_t)-e->error_code);
      } else {
        sd_dec((uint64_t)e->error_code);
      }
      const char *en = sysdiag_lookup_errno_name(e->error_code);
      if (en) {
        sd_put(" (");
        sd_put(en);
        sd_put(")");
      }
      sd_put("\n");
    }
  }

  /* Frequent / Recent Syscall Errors */
  if (g_syscall_error_count > 0) {
    sd_put("[SYSDIAG] recent / frequent syscall errors (tracked keys: ");
    sd_dec(g_syscall_error_count);
    sd_put(", total errors: ");
    sd_dec(g_syscall_error_total_calls);
    sd_put(", evicted keys: ");
    sd_dec(g_syscall_error_evictions);
    sd_put("):\n");
    for (size_t i = 0; i < g_syscall_error_count; i++) {
      sysdiag_syscall_error_t *e = &g_syscall_errors[i];
      sd_put("[SYSDIAG]   #");
      sd_dec(i + 1);
      sd_put(": ");
      sd_put(e->name);
      sd_put("(#");
      sd_dec(e->syscall_num);
      sd_put(") -> -");
      sd_dec((uint64_t)-e->error_code);
      const char *en = sysdiag_lookup_errno_name(e->error_code);
      if (en) {
        sd_put(" (");
        sd_put(en);
        sd_put(")");
      }
      if (e->via_seccomp)
        sd_put(" via-seccomp-deny");
      sd_put(" count=");
      sd_dec(e->total_calls);
      sd_put(" comm='");
      sd_put(e->comm[0] ? e->comm : "?");
      sd_put("' pid=");
      sd_dec(e->last_tgid);
      sd_put(" rip=");
      sd_hex64(e->last_rip);
      sd_put(" args=(");
      sd_hex64(e->last_args[0]);
      sd_put(", ");
      sd_hex64(e->last_args[1]);
      sd_put(", ");
      sd_hex64(e->last_args[2]);
      sd_put(")");
      if (e->syscall_num == 45) {
        sd_put(" recv-flags=");
        sd_hex64(e->last_args[3]);
      }
      if (e->detail[0]) {
        sd_put(" detail='");
        sd_put(e->detail);
        sd_put("'");
      }
      if (e->path[0]) {
        sd_put(" path='");
        sd_put(e->path);
        sd_put("'");
      }
      sd_put("\n");
    }
    /* Join-stall signatures: re-list the entries that matter when a game
     * client wedges on entry. Game-client comms plus the error codes the
     * audit trail ties to join (extent EIO, no-space, cross-device rename,
     * peer timeout) so they are not buried under 128 startup probes.
     * Normal event-loop results (EAGAIN/EINTR/timeout) are excluded here. */
    {
      int sig_n = 0;
      for (size_t i = 0; i < g_syscall_error_count; i++) {
        const sysdiag_syscall_error_t *e = &g_syscall_errors[i];
        if (sysdiag_err_is_join_signature(e))
          sig_n++;
      }
      sd_put("[SYSDIAG] join investigation candidates (game comms + I/O errors; excludes EAGAIN/EINTR/futex timeouts, ");
      sd_dec((uint64_t)sig_n);
      sd_put(" matching):\n");
      if (!sig_n) {
        sd_put("[SYSDIAG]   (none: no game-client or join-error keys recorded)\n");
      } else {
        for (size_t i = 0; i < g_syscall_error_count; i++) {
          const sysdiag_syscall_error_t *e = &g_syscall_errors[i];
          if (!sysdiag_err_is_join_signature(e))
            continue;
          sd_put("[SYSDIAG]   sig: ");
          sd_put(e->name);
          sd_put("(#");
          sd_dec(e->syscall_num);
          sd_put(") -> -");
          sd_dec((uint64_t)-e->error_code);
          sd_put(" count=");
          sd_dec(e->total_calls);
          sd_put(" comm='");
          sd_put(e->comm[0] ? e->comm : "?");
          sd_put("' pid=");
          sd_dec(e->last_tgid);
          sd_put(" rip=");
          sd_hex64(e->last_rip);
          if (e->path[0]) {
            sd_put(" path='");
            sd_put(e->path);
            sd_put("'");
          }
          sd_put("\n");
        }
      }
    }
  }

  spinlock_release(&g_sysdiag_lock);
  sd_put("[SYSDIAG] =============================================================\n");
}

/* Helper to write string to buffer with bounds checking */
static size_t b_write(char *dst, size_t dst_sz, size_t pos, const char *src) {
  if (!src) return pos;
  while (*src && pos + 1 < dst_sz) {
    dst[pos++] = *src++;
  }
  dst[pos] = '\0';
  return pos;
}

static size_t b_dec(char *dst, size_t dst_sz, size_t pos, uint64_t v) {
  char buf[24];
  int i = 0;
  do {
    buf[i++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v && i < 23);
  while (i > 0 && pos + 1 < dst_sz) {
    dst[pos++] = buf[--i];
  }
  dst[pos] = '\0';
  return pos;
}

static size_t b_hex32(char *dst, size_t dst_sz, size_t pos, uint32_t v) {
  const char *h = "0123456789ABCDEF";
  for (int i = 7; i >= 0 && pos + 1 < dst_sz; i--) {
    dst[pos++] = h[(v >> (i * 4)) & 0xF];
  }
  dst[pos] = '\0';
  return pos;
}

static size_t b_hex64(char *dst, size_t dst_sz, size_t pos, uint64_t v) {
  const char *h = "0123456789ABCDEF";
  for (int i = 15; i >= 0 && pos + 1 < dst_sz; i--) {
    dst[pos++] = h[(v >> (i * 4)) & 0xF];
  }
  dst[pos] = '\0';
  return pos;
}

size_t sysdiag_format_report(char *buf, size_t max_size) {
  if (!buf || max_size == 0) return 0;
  size_t p = 0;

  p = b_write(buf, max_size, p, "=== AvoryOS Compatibility & Missing Syscall/Ioctl Diagnostics ===\n\n");

  spinlock_acquire(&g_sysdiag_lock);

  /* Missing syscalls */
  p = b_write(buf, max_size, p, "Missing Syscalls (unique: ");
  p = b_dec(buf, max_size, p, g_syscall_miss_count);
  p = b_write(buf, max_size, p, ", total: ");
  p = b_dec(buf, max_size, p, g_syscall_miss_total_calls);
  p = b_write(buf, max_size, p, "):\n");

  if (g_syscall_miss_count == 0) {
    p = b_write(buf, max_size, p, "  (none recorded)\n");
  } else {
    for (size_t i = 0; i < g_syscall_miss_count && p + 128 < max_size; i++) {
      sysdiag_syscall_entry_t *e = &g_syscall_misses[i];
      p = b_write(buf, max_size, p, "  #");
      p = b_dec(buf, max_size, p, i + 1);
      p = b_write(buf, max_size, p, ": syscall ");
      p = b_dec(buf, max_size, p, e->syscall_num);
      p = b_write(buf, max_size, p, " (");
      p = b_write(buf, max_size, p, e->name);
      p = b_write(buf, max_size, p, ") count=");
      p = b_dec(buf, max_size, p, e->total_calls);
      p = b_write(buf, max_size, p, " comm='");
      p = b_write(buf, max_size, p, e->comm[0] ? e->comm : "?");
      p = b_write(buf, max_size, p, "' pid=");
      p = b_dec(buf, max_size, p, e->last_tgid);
      p = b_write(buf, max_size, p, " rip=0x");
      p = b_hex64(buf, max_size, p, e->last_rip);
      p = b_write(buf, max_size, p, " args=(0x");
      p = b_hex64(buf, max_size, p, e->last_args[0]);
      p = b_write(buf, max_size, p, ", 0x");
      p = b_hex64(buf, max_size, p, e->last_args[1]);
      p = b_write(buf, max_size, p, ")\n");
    }
  }

  p = b_write(buf, max_size, p, "\nUnhandled Ioctls (unique: ");
  p = b_dec(buf, max_size, p, g_ioctl_miss_count);
  p = b_write(buf, max_size, p, ", total: ");
  p = b_dec(buf, max_size, p, g_ioctl_miss_total_calls);
  p = b_write(buf, max_size, p, "):\n");

  if (g_ioctl_miss_count == 0) {
    p = b_write(buf, max_size, p, "  (none recorded)\n");
  } else {
    for (size_t i = 0; i < g_ioctl_miss_count && p + 128 < max_size; i++) {
      sysdiag_ioctl_entry_t *e = &g_ioctl_misses[i];
      p = b_write(buf, max_size, p, "  #");
      p = b_dec(buf, max_size, p, i + 1);
      p = b_write(buf, max_size, p, ": ioctl 0x");
      p = b_hex32(buf, max_size, p, e->request);
      p = b_write(buf, max_size, p, " (");
      p = b_write(buf, max_size, p, e->name);
      p = b_write(buf, max_size, p, ") node='");
      p = b_write(buf, max_size, p, e->node_name[0] ? e->node_name : "anon");
      p = b_write(buf, max_size, p, "' count=");
      p = b_dec(buf, max_size, p, e->total_calls);
      p = b_write(buf, max_size, p, " comm='");
      p = b_write(buf, max_size, p, e->comm[0] ? e->comm : "?");
      p = b_write(buf, max_size, p, "' pid=");
      p = b_dec(buf, max_size, p, e->last_tgid);
      p = b_write(buf, max_size, p, " fd=");
      p = b_dec(buf, max_size, p, e->fd);
      p = b_write(buf, max_size, p, " arg=0x");
      p = b_hex64(buf, max_size, p, e->last_arg);
      p = b_write(buf, max_size, p, e->handler_called ? " [returned ENOTTY]\n" : " [no handler]\n");
    }
  }

  if (g_feature_miss_count > 0) {
    p = b_write(buf, max_size, p, "\nUnimplemented Features (unique: ");
    p = b_dec(buf, max_size, p, g_feature_miss_count);
    p = b_write(buf, max_size, p, "):\n");
    for (size_t i = 0; i < g_feature_miss_count && p + 128 < max_size; i++) {
      sysdiag_feature_entry_t *e = &g_feature_misses[i];
      p = b_write(buf, max_size, p, "  #");
      p = b_dec(buf, max_size, p, i + 1);
      p = b_write(buf, max_size, p, ": ");
      p = b_write(buf, max_size, p, e->subsystem);
      p = b_write(buf, max_size, p, "::");
      p = b_write(buf, max_size, p, e->feature);
      p = b_write(buf, max_size, p, " count=");
      p = b_dec(buf, max_size, p, e->total_calls);
      p = b_write(buf, max_size, p, " comm='");
      p = b_write(buf, max_size, p, e->comm[0] ? e->comm : "?");
      p = b_write(buf, max_size, p, "' pid=");
      p = b_dec(buf, max_size, p, e->last_tgid);
      p = b_write(buf, max_size, p, "\n");
    }
  }

  if (g_fs_miss_count > 0) {
    p = b_write(buf, max_size, p, "\nMissing /proc & /sys Nodes (unique: ");
    p = b_dec(buf, max_size, p, g_fs_miss_count);
    p = b_write(buf, max_size, p, ", total: ");
    p = b_dec(buf, max_size, p, g_fs_miss_total_calls);
    p = b_write(buf, max_size, p, "):\n");
    for (size_t i = 0; i < g_fs_miss_count && p + 128 < max_size; i++) {
      sysdiag_fs_entry_t *e = &g_fs_misses[i];
      p = b_write(buf, max_size, p, "  #");
      p = b_dec(buf, max_size, p, i + 1);
      p = b_write(buf, max_size, p, ": [");
      p = b_write(buf, max_size, p, e->op);
      p = b_write(buf, max_size, p, "] '");
      p = b_write(buf, max_size, p, e->path);
      p = b_write(buf, max_size, p, "' hits=");
      p = b_dec(buf, max_size, p, e->total_calls);
      p = b_write(buf, max_size, p, " comm='");
      p = b_write(buf, max_size, p, e->comm[0] ? e->comm : "?");
      p = b_write(buf, max_size, p, "' pid=");
      p = b_dec(buf, max_size, p, e->last_tgid);
      p = b_write(buf, max_size, p, " ret=");
      if (e->error_code < 0) {
        p = b_write(buf, max_size, p, "-");
        p = b_dec(buf, max_size, p, (uint64_t)-e->error_code);
      } else {
        p = b_dec(buf, max_size, p, (uint64_t)e->error_code);
      }
      const char *en = sysdiag_lookup_errno_name(e->error_code);
      if (en) {
        p = b_write(buf, max_size, p, " (");
        p = b_write(buf, max_size, p, en);
        p = b_write(buf, max_size, p, ")");
      }
      p = b_write(buf, max_size, p, "\n");
    }
  }

  if (g_syscall_error_count > 0) {
    p = b_write(buf, max_size, p, "\nRecent / Frequent Syscall Errors (unique: ");
    p = b_dec(buf, max_size, p, g_syscall_error_count);
    p = b_write(buf, max_size, p, ", total: ");
    p = b_dec(buf, max_size, p, g_syscall_error_total_calls);
    p = b_write(buf, max_size, p, "):\n");
    for (size_t i = 0; i < g_syscall_error_count && p + 256 < max_size; i++) {
      sysdiag_syscall_error_t *e = &g_syscall_errors[i];
      p = b_write(buf, max_size, p, "  #");
      p = b_dec(buf, max_size, p, i + 1);
      p = b_write(buf, max_size, p, ": ");
      p = b_write(buf, max_size, p, e->name);
      p = b_write(buf, max_size, p, "(#");
      p = b_dec(buf, max_size, p, e->syscall_num);
      p = b_write(buf, max_size, p, ") -> -");
      p = b_dec(buf, max_size, p, (uint64_t)-e->error_code);
      const char *en = sysdiag_lookup_errno_name(e->error_code);
      if (en) {
        p = b_write(buf, max_size, p, " (");
        p = b_write(buf, max_size, p, en);
        p = b_write(buf, max_size, p, ")");
      }
      if (e->via_seccomp)
        p = b_write(buf, max_size, p, " via-seccomp-deny");
      p = b_write(buf, max_size, p, " count=");
      p = b_dec(buf, max_size, p, e->total_calls);
      p = b_write(buf, max_size, p, " comm='");
      p = b_write(buf, max_size, p, e->comm[0] ? e->comm : "?");
      p = b_write(buf, max_size, p, "' pid=");
      p = b_dec(buf, max_size, p, e->last_tgid);
      p = b_write(buf, max_size, p, " rip=0x");
      p = b_hex64(buf, max_size, p, e->last_rip);
      p = b_write(buf, max_size, p, " args=(0x");
      p = b_hex64(buf, max_size, p, e->last_args[0]);
      p = b_write(buf, max_size, p, ", 0x");
      p = b_hex64(buf, max_size, p, e->last_args[1]);
      p = b_write(buf, max_size, p, ", 0x");
      p = b_hex64(buf, max_size, p, e->last_args[2]);
      p = b_write(buf, max_size, p, ")");
      if (e->detail[0]) {
        p = b_write(buf, max_size, p, " detail='");
        p = b_write(buf, max_size, p, e->detail);
        p = b_write(buf, max_size, p, "'");
      }
      if (e->path[0]) {
        p = b_write(buf, max_size, p, " path='");
        p = b_write(buf, max_size, p, e->path);
        p = b_write(buf, max_size, p, "'");
      }
      p = b_write(buf, max_size, p, "\n");
    }
  }

  spinlock_release(&g_sysdiag_lock);
  return p;
}
