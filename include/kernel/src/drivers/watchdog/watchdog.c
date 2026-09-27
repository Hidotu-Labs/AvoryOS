#include "watchdog.h"
#include "../../acpi/acpi.h"
#include "../../apic/lapic_timer.h"
#include "../../console/klog.h"
#include "../../fs/devfs.h"
#include "../../fs/vfs.h"
#include "../../lib/string.h"
#include "../../lock/spinlock.h"
#include "../../mm/heap.h"
#include "../../mm/vmm.h"

#define WATCHDOG_DEFAULT_TIMEOUT 60
#define WATCHDOG_MIN_TIMEOUT     1
#define WATCHDOG_MAX_TIMEOUT     3600

typedef struct {
  spinlock_t lock;
  int timeout_seconds;
  int pretimeout_seconds;
  uint64_t expires_ms;
  bool active;
  bool magic_close;
  bool open;
  uint32_t bootstatus;
} watchdog_dev_t;

static watchdog_dev_t g_watchdog;

static void watchdog_ping_locked(void) {
  g_watchdog.expires_ms = lapic_timer_get_ms() + (uint64_t)g_watchdog.timeout_seconds * 1000ULL;
}

static void watchdog_vfs_open(vfs_node_t *node) {
  (void)node;
  spinlock_acquire(&g_watchdog.lock);
  g_watchdog.open = true;
  g_watchdog.active = true;
  g_watchdog.magic_close = false;
  watchdog_ping_locked();
  spinlock_release(&g_watchdog.lock);
}

static void watchdog_vfs_close(vfs_node_t *node) {
  (void)node;
  spinlock_acquire(&g_watchdog.lock);
  g_watchdog.open = false;
  if (g_watchdog.magic_close) {
    g_watchdog.active = false;
    klog_puts(KLOG_CLR_YELLOW "[WATCHDOG]" KLOG_CLR_RESET " Watchdog safely disarmed via magic close.\n");
  } else {
    klog_puts(KLOG_CLR_YELLOW "[WATCHDOG]" KLOG_CLR_RESET " Watchdog closed without magic character; timer remains active!\n");
  }
  g_watchdog.magic_close = false;
  spinlock_release(&g_watchdog.lock);
}

static uint32_t watchdog_vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (!buffer || size == 0) return 0;

  spinlock_acquire(&g_watchdog.lock);
  // Check for the magic close character 'V'
  for (uint32_t i = 0; i < size; i++) {
    if (buffer[i] == 'V') {
      g_watchdog.magic_close = true;
    }
  }
  // Any write acts as a keepalive / ping
  watchdog_ping_locked();
  spinlock_release(&g_watchdog.lock);

  return size;
}

static int watchdog_vfs_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg) {
  (void)node;
  int ret = 0;

  spinlock_acquire(&g_watchdog.lock);

  switch (request) {
  case WDIOC_GETSUPPORT: {
    if (!arg) {
      ret = -14; // EFAULT
      break;
    }
    struct watchdog_info info;
    memset(&info, 0, sizeof(info));
    info.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE;
    info.firmware_version = 1;
    strncpy((char *)info.identity, "AvoryOS Watchdog", sizeof(info.identity) - 1);

    memcpy((void *)arg, &info, sizeof(info));
    ret = 0;
    break;
  }

  case WDIOC_GETSTATUS: {
    if (!arg) {
      ret = -14;
      break;
    }
    int status = g_watchdog.active ? (WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT) : 0;
    *(int *)arg = status;
    ret = 0;
    break;
  }

  case WDIOC_GETBOOTSTATUS: {
    if (!arg) {
      ret = -14;
      break;
    }
    *(int *)arg = (int)g_watchdog.bootstatus;
    ret = 0;
    break;
  }

  case WDIOC_GETTEMP: {
    ret = -95; // EOPNOTSUPP
    break;
  }

  case WDIOC_SETOPTIONS: {
    if (!arg) {
      ret = -14;
      break;
    }
    int options = *(int *)arg;
    if (options & WDIOS_DISABLECARD) {
      g_watchdog.active = false;
      klog_puts(KLOG_CLR_YELLOW "[WATCHDOG]" KLOG_CLR_RESET " Watchdog disabled via ioctl.\n");
    }
    if (options & WDIOS_ENABLECARD) {
      g_watchdog.active = true;
      watchdog_ping_locked();
      klog_puts(KLOG_CLR_GREEN "[WATCHDOG]" KLOG_CLR_RESET " Watchdog enabled via ioctl.\n");
    }
    ret = 0;
    break;
  }

  case WDIOC_KEEPALIVE: {
    watchdog_ping_locked();
    ret = 0;
    break;
  }

  case WDIOC_SETTIMEOUT: {
    if (!arg) {
      ret = -14;
      break;
    }
    int new_timeout = *(int *)arg;
    if (new_timeout < WATCHDOG_MIN_TIMEOUT || new_timeout > WATCHDOG_MAX_TIMEOUT) {
      ret = -22; // EINVAL
      break;
    }
    g_watchdog.timeout_seconds = new_timeout;
    watchdog_ping_locked();
    *(int *)arg = g_watchdog.timeout_seconds;
    ret = 0;
    break;
  }

  case WDIOC_GETTIMEOUT: {
    if (!arg) {
      ret = -14;
      break;
    }
    *(int *)arg = g_watchdog.timeout_seconds;
    ret = 0;
    break;
  }

  case WDIOC_SETPRETIMEOUT: {
    if (!arg) {
      ret = -14;
      break;
    }
    int new_pretimeout = *(int *)arg;
    if (new_pretimeout < 0 || new_pretimeout >= g_watchdog.timeout_seconds) {
      ret = -22;
      break;
    }
    g_watchdog.pretimeout_seconds = new_pretimeout;
    *(int *)arg = g_watchdog.pretimeout_seconds;
    ret = 0;
    break;
  }

  case WDIOC_GETPRETIMEOUT: {
    if (!arg) {
      ret = -14;
      break;
    }
    *(int *)arg = g_watchdog.pretimeout_seconds;
    ret = 0;
    break;
  }

  case WDIOC_GETTIMELEFT: {
    if (!arg) {
      ret = -14;
      break;
    }
    uint64_t now = lapic_timer_get_ms();
    int timeleft = 0;
    if (g_watchdog.active && g_watchdog.expires_ms > now) {
      timeleft = (int)((g_watchdog.expires_ms - now + 999ULL) / 1000ULL);
    }
    *(int *)arg = timeleft;
    ret = 0;
    break;
  }

  default:
    ret = -25; // ENOTTY
    break;
  }

  spinlock_release(&g_watchdog.lock);
  return ret;
}

void watchdog_tick(void) {
  if (!g_watchdog.active) return;

  uint64_t now = lapic_timer_get_ms();
  if (now >= g_watchdog.expires_ms) {
    g_watchdog.active = false;
    klog_puts("\n" KLOG_CLR_RED "################################################################################" KLOG_CLR_RESET "\n");
    klog_puts(KLOG_CLR_RED "[ WATCHDOG TIMEOUT ]" KLOG_CLR_RESET " Watchdog timer expired! Initiating hardware reset...\n");
    klog_puts(KLOG_CLR_RED "################################################################################" KLOG_CLR_RESET "\n\n");
    acpi_reboot();
  }
}

void watchdog_init(void) {
  memset(&g_watchdog, 0, sizeof(g_watchdog));
  spinlock_init(&g_watchdog.lock);
  g_watchdog.timeout_seconds = WATCHDOG_DEFAULT_TIMEOUT;
  g_watchdog.active = false;
  g_watchdog.magic_close = false;
  g_watchdog.open = false;
  g_watchdog.bootstatus = 0;

  vfs_node_t *dev_dir = vfs_resolve_path("/dev");

  devfs_setup_chardev(dev_dir, "watchdog",
                      NULL, watchdog_vfs_write,
                      watchdog_vfs_open, watchdog_vfs_close,
                      NULL, watchdog_vfs_ioctl,
                      NULL, &g_watchdog, 0, 0);

  devfs_setup_chardev(dev_dir, "watchdog0",
                      NULL, watchdog_vfs_write,
                      watchdog_vfs_open, watchdog_vfs_close,
                      NULL, watchdog_vfs_ioctl,
                      NULL, &g_watchdog, 0, 0);

  if (dev_dir)
    vfs_close(dev_dir);

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Watchdog driver initialized (/dev/watchdog, /dev/watchdog0, default=60s)\n");
}
