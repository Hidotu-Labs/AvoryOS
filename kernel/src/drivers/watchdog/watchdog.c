
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
#include "arch/uaccess.h"

#define WATCHDOG_DEFAULT_TIMEOUT 60
#define WATCHDOG_MIN_TIMEOUT     1
#define WATCHDOG_MAX_TIMEOUT     3600

/* Longest keepalive write scanned for the magic 'V'.  Keepalive pings are a
 * few bytes; the cap keeps a huge userspace write from pinning this CPU. */
#define WATCHDOG_MAGIC_SCAN_MAX 4096
#define WATCHDOG_SCAN_CHUNK     32

/* Per-open descriptor state.  The node handed to the fd table points at one
 * of these through node->device. */
typedef struct watchdog_client {
  bool magic_close; /* 'V' seen on this descriptor */
  struct watchdog_client *next;
} watchdog_client_t;

typedef struct {
  spinlock_t lock;
  int timeout_seconds;
  int pretimeout_seconds;
  uint64_t expires_ms;
  bool active;
  uint32_t bootstatus;
  uint32_t open_count;
  /* Set when a client saw 'V'; consumed when the last client closes. */
  bool magic_close;
  watchdog_client_t *clients;
} watchdog_dev_t;

static watchdog_dev_t g_watchdog;

static void watchdog_vfs_close(vfs_node_t *node);

static void watchdog_ping_locked(void) {
  g_watchdog.expires_ms =
      lapic_timer_get_ms() + (uint64_t)g_watchdog.timeout_seconds * 1000ULL;
}

/* Scan a user keepalive buffer in bounded chunks.  Returns 0 on success
 * (setting *magic if 'V' was seen) or -14/EFAULT if the range is not
 * readable.  copy_from_user() consumes the whole chunk or reports failure,
 * so a chunk straddling an unmapped page is rejected cleanly. */
static int watchdog_scan_magic_user(const uint8_t *buffer, uint32_t size,
                                    bool *magic) {
  uint8_t chunk[WATCHDOG_SCAN_CHUNK];
  uint32_t limit =
      size > WATCHDOG_MAGIC_SCAN_MAX ? WATCHDOG_MAGIC_SCAN_MAX : size;
  uint32_t scanned = 0;

  while (scanned < limit) {
    uint32_t want = limit - scanned;
    if (want > sizeof(chunk))
      want = sizeof(chunk);
    if (copy_from_user(chunk, buffer + scanned, want) != 0)
      return -14;
    for (uint32_t i = 0; i < want; i++) {
      if (chunk[i] == 'V')
        *magic = true;
    }
    scanned += want;
  }
  return 0;
}

static uint32_t watchdog_vfs_write(vfs_node_t *node, uint32_t offset,
                                   uint32_t size, uint8_t *buffer) {
  (void)offset;
  if (!buffer || size == 0)
    return 0;

  watchdog_client_t *client =
      node ? (watchdog_client_t *)node->device : NULL;

  bool magic = false;
  if (is_user_ptr((uint64_t)buffer)) {
    if (watchdog_scan_magic_user(buffer, size, &magic) != 0)
      return (uint32_t)-14; /* EFAULT */
  } else {
    /* Kernel caller (driver-to-driver or a node write): plain buffer. */
    uint32_t limit =
        size > WATCHDOG_MAGIC_SCAN_MAX ? WATCHDOG_MAGIC_SCAN_MAX : size;
    for (uint32_t i = 0; i < limit; i++) {
      if (buffer[i] == 'V') {
        magic = true;
        break;
      }
    }
  }

  spinlock_acquire(&g_watchdog.lock);
  if (client)
    client->magic_close = client->magic_close || magic;
  else
    g_watchdog.magic_close = g_watchdog.magic_close || magic;
  watchdog_ping_locked();
  spinlock_release(&g_watchdog.lock);

  return size;
}

/* Copy one int out of the driver into userspace.  Returns 0 or -14/EFAULT. */
static int watchdog_copy_out_int(uint64_t arg, int value) {
  if (!arg || !is_user_range((void *)arg, sizeof(int)))
    return -14;
  return copy_to_user((void *)arg, &value, sizeof(int)) ? -14 : 0;
}

static int watchdog_vfs_ioctl(vfs_node_t *node, uint32_t request, uint64_t arg) {
  (void)node;
  int ret = 0;

  switch (request) {
  case WDIOC_GETSUPPORT: {
    if (!arg || !is_user_range((void *)arg, sizeof(struct watchdog_info)))
      return -14;
    struct watchdog_info info;
    memset(&info, 0, sizeof(info));
    info.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE;
    info.firmware_version = 1;
    strncpy((char *)info.identity, "AvoryOS Watchdog", sizeof(info.identity) - 1);
    if (copy_to_user((void *)arg, &info, sizeof(info)))
      return -14;
    return 0;
  }

  case WDIOC_GETSTATUS: {
    int status;
    spinlock_acquire(&g_watchdog.lock);
    status = g_watchdog.active ? (WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT) : 0;
    spinlock_release(&g_watchdog.lock);
    return watchdog_copy_out_int(arg, status);
  }

  case WDIOC_GETBOOTSTATUS: {
    int bootstatus;
    spinlock_acquire(&g_watchdog.lock);
    bootstatus = (int)g_watchdog.bootstatus;
    spinlock_release(&g_watchdog.lock);
    return watchdog_copy_out_int(arg, bootstatus);
  }

  case WDIOC_GETTEMP:
    return -95; // EOPNOTSUPP

  case WDIOC_SETOPTIONS: {
    if (!arg || !is_user_range((void *)arg, sizeof(int)))
      return -14;
    int options;
    if (copy_from_user(&options, (const void *)arg, sizeof(options)))
      return -14;

    bool disabled = false, enabled = false;
    spinlock_acquire(&g_watchdog.lock);
    if (options & WDIOS_DISABLECARD) {
      g_watchdog.active = false;
      g_watchdog.magic_close = false;
      disabled = true;
    }
    if (options & WDIOS_ENABLECARD) {
      g_watchdog.active = true;
      watchdog_ping_locked();
      enabled = true;
    }
    spinlock_release(&g_watchdog.lock);

    if (disabled)
      klog_puts(KLOG_CLR_YELLOW "[WATCHDOG]" KLOG_CLR_RESET
                " Watchdog disabled via ioctl.\n");
    if (enabled)
      klog_puts(KLOG_CLR_GREEN "[WATCHDOG]" KLOG_CLR_RESET
                " Watchdog enabled via ioctl.\n");
    return 0;
  }

  case WDIOC_KEEPALIVE:
    /* The argument is not a pointer for this request: ignore it. */
    spinlock_acquire(&g_watchdog.lock);
    watchdog_ping_locked();
    spinlock_release(&g_watchdog.lock);
    return 0;

  case WDIOC_SETTIMEOUT: {
    if (!arg || !is_user_range((void *)arg, sizeof(int)))
      return -14;
    int new_timeout;
    if (copy_from_user(&new_timeout, (const void *)arg, sizeof(new_timeout)))
      return -14;
    if (new_timeout < WATCHDOG_MIN_TIMEOUT || new_timeout > WATCHDOG_MAX_TIMEOUT)
      return -22; // EINVAL

    spinlock_acquire(&g_watchdog.lock);
    g_watchdog.timeout_seconds = new_timeout;
    watchdog_ping_locked();
    spinlock_release(&g_watchdog.lock);

    /* Linux returns the resulting timeout through the same pointer. */
    return watchdog_copy_out_int(arg, new_timeout);
  }

  case WDIOC_GETTIMEOUT: {
    int timeout;
    spinlock_acquire(&g_watchdog.lock);
    timeout = g_watchdog.timeout_seconds;
    spinlock_release(&g_watchdog.lock);
    return watchdog_copy_out_int(arg, timeout);
  }

  case WDIOC_SETPRETIMEOUT: {
    if (!arg || !is_user_range((void *)arg, sizeof(int)))
      return -14;
    int new_pretimeout;
    if (copy_from_user(&new_pretimeout, (const void *)arg,
                       sizeof(new_pretimeout)))
      return -14;

    spinlock_acquire(&g_watchdog.lock);
    if (new_pretimeout < 0 || new_pretimeout >= g_watchdog.timeout_seconds) {
      spinlock_release(&g_watchdog.lock);
      return -22; // EINVAL
    }
    g_watchdog.pretimeout_seconds = new_pretimeout;
    spinlock_release(&g_watchdog.lock);

    return watchdog_copy_out_int(arg, new_pretimeout);
  }

  case WDIOC_GETPRETIMEOUT: {
    int pretimeout;
    spinlock_acquire(&g_watchdog.lock);
    pretimeout = g_watchdog.pretimeout_seconds;
    spinlock_release(&g_watchdog.lock);
    return watchdog_copy_out_int(arg, pretimeout);
  }

  case WDIOC_GETTIMELEFT: {
    int timeleft = 0;
    spinlock_acquire(&g_watchdog.lock);
    if (g_watchdog.active) {
      uint64_t now = lapic_timer_get_ms();
      if (g_watchdog.expires_ms > now)
        timeleft = (int)((g_watchdog.expires_ms - now + 999ULL) / 1000ULL);
    }
    spinlock_release(&g_watchdog.lock);
    return watchdog_copy_out_int(arg, timeleft);
  }

  default:
    ret = -25; // ENOTTY
    break;
  }

  return ret;
}

void watchdog_tick(void) {
  /* Cheap unlocked peek first: the timer runs this every tick and the
   * watchdog is armed only while a client has it open. */
  if (!__atomic_load_n(&g_watchdog.active, __ATOMIC_RELAXED))
    return;

  bool expired = false;
  spinlock_acquire(&g_watchdog.lock);
  if (g_watchdog.active &&
      lapic_timer_get_ms() >= g_watchdog.expires_ms) {
    g_watchdog.active = false;
    g_watchdog.magic_close = false;
    expired = true;
  }
  spinlock_release(&g_watchdog.lock);

  if (!expired)
    return;

  klog_puts("\n" KLOG_CLR_RED
            "################################################################################"
            KLOG_CLR_RESET "\n");
  klog_puts(KLOG_CLR_RED "[ WATCHDOG TIMEOUT ]" KLOG_CLR_RESET
            " Watchdog timer expired! Initiating hardware reset...\n");
  klog_puts(KLOG_CLR_RED
            "################################################################################"
            KLOG_CLR_RESET "\n\n");
  acpi_reboot();
}

/* Per-open descriptor factory: arms the timer and gives the descriptor its
 * own magic-close state.  SYS_OPEN converts a NULL return to -ENOMEM. */
static vfs_node_t *watchdog_open_instance(vfs_node_t *metadata) {
  watchdog_client_t *client = kmalloc(sizeof(*client));
  if (!client)
    return NULL;
  memset(client, 0, sizeof(*client));

  vfs_node_t *node = kmalloc(sizeof(*node));
  if (!node) {
    kfree(client);
    return NULL;
  }
  vfs_node_init(node);
  strncpy(node->name, metadata ? metadata->name : "watchdog",
          sizeof(node->name) - 1);
  node->name[sizeof(node->name) - 1] = '\0';
  node->flags = FS_CHARDEV;
  node->mask = 0666;
  node->device = client;
  node->write = watchdog_vfs_write;
  node->ioctl = watchdog_vfs_ioctl;
  node->close = watchdog_vfs_close;
  node->refcount = 0; /* the descriptor owns the initial reference */

  spinlock_acquire(&g_watchdog.lock);
  client->next = g_watchdog.clients;
  g_watchdog.clients = client;
  g_watchdog.open_count++;
  g_watchdog.active = true;
  watchdog_ping_locked();
  spinlock_release(&g_watchdog.lock);

  return node;
}

/* Runs when the descriptor's last reference drops.  Only now can the timer
 * be disarmed: another descriptor may still be feeding it. */
static void watchdog_vfs_close(vfs_node_t *node) {
  watchdog_client_t *client =
      node ? (watchdog_client_t *)node->device : NULL;
  if (node)
    node->device = NULL;

  bool disarm = false;
  bool still_armed = false;
  uint32_t remaining = 0;

  spinlock_acquire(&g_watchdog.lock);

  if (client) {
    watchdog_client_t **pp = &g_watchdog.clients;
    while (*pp) {
      if (*pp == client) {
        *pp = client->next;
        break;
      }
      pp = &(*pp)->next;
    }
    if (client->magic_close)
      g_watchdog.magic_close = true;
    if (g_watchdog.open_count)
      g_watchdog.open_count--;
  }
  remaining = g_watchdog.open_count;

  if (remaining == 0) {
    if (g_watchdog.magic_close && g_watchdog.active) {
      g_watchdog.active = false;
      disarm = true;
    }
    still_armed = g_watchdog.active;
    g_watchdog.magic_close = false;
  }

  spinlock_release(&g_watchdog.lock);

  kfree(client);

  if (disarm) {
    klog_puts(KLOG_CLR_YELLOW "[WATCHDOG]" KLOG_CLR_RESET
              " Watchdog safely disarmed via magic close.\n");
  } else if (remaining == 0 && still_armed) {
    klog_puts(KLOG_CLR_YELLOW "[WATCHDOG]" KLOG_CLR_RESET
              " Watchdog closed without magic character; timer remains active!\n");
  }
}

void watchdog_init(void) {
  memset(&g_watchdog, 0, sizeof(g_watchdog));
  spinlock_init(&g_watchdog.lock);
  g_watchdog.timeout_seconds = WATCHDOG_DEFAULT_TIMEOUT;
  g_watchdog.active = false;
  g_watchdog.magic_close = false;
  g_watchdog.open_count = 0;
  g_watchdog.clients = NULL;
  g_watchdog.bootstatus = 0;

  vfs_node_t *dev_dir = vfs_resolve_path("/dev");

  /* The registered nodes are metadata only: the real per-open node comes
   * from watchdog_open_instance(), which is where arming and per-descriptor
   * magic-close state live. */
  devfs_setup_chardev(dev_dir, "watchdog",
                      NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                      NULL, 0, 0);
  devfs_setup_chardev(dev_dir, "watchdog0",
                      NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                      NULL, 0, 0);

  vfs_node_t *meta = devfs_lookup("watchdog");
  if (meta)
    meta->open_instance = watchdog_open_instance;
  meta = devfs_lookup("watchdog0");
  if (meta)
    meta->open_instance = watchdog_open_instance;

  if (dev_dir)
    vfs_close(dev_dir);

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET
            " Watchdog driver initialized (/dev/watchdog, /dev/watchdog0, default=60s)\n");
}
