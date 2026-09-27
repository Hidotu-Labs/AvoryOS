// evdev.c — Linux-compatible evdev input subsystem for X11
//
// Provides /dev/input/event0 (keyboard) and /dev/input/event1 (mouse) device
// nodes that speak the standard Linux evdev protocol. This allows unmodified
// Xorg/kdrive to discover and read input devices.
//
// Events are pushed from the PS/2 keyboard and mouse IRQ handlers into
// per-device ring buffers, then delivered to userspace via read() as
// struct input_event packets (24 bytes each on x86_64).
//
// The ioctl() interface implements the EVIOC* commands that Xorg's evdev
// driver uses during device probing (EVIOCGVERSION, EVIOCGID, EVIOCGNAME,
// EVIOCGBIT, EVIOCGABS).

#include "drivers/input/evdev.h"
#include "../../apic/lapic_timer.h"
#include "../../console/klog.h"
#include "../../drivers/timer/rtc.h"
#include "../../fb/framebuffer.h"
#include "../../fs/ramfs.h"
#include "../../lock/lockdiag.h"
#include "../../fs/vfs.h"
#include "../../lib/string.h"
#include "../../lock/spinlock.h"
#include "../../mm/heap.h"
#include "../../sched/sched.h"
#include "../../sched/wait.h"
#include "../../socket/epoll.h"
#include <stdbool.h>
#include <stdint.h>

// Two global evdev devices
static evdev_device_t kbd_evdev;
static evdev_device_t mouse_evdev;
static bool evdev_ready;

evdev_device_t *evdev_get_keyboard(void) {
  return evdev_ready ? &kbd_evdev : NULL;
}
evdev_device_t *evdev_get_mouse(void) {
  return evdev_ready ? &mouse_evdev : NULL;
}

// Timestamp helper
extern uint64_t pit_get_ticks(void);

static void evdev_timestamp(uint64_t *sec, uint64_t *usec) {
  uint64_t ticks = pit_get_ticks();
  *sec = ticks / 100; // PIT runs at 100 Hz
  *usec = (ticks % 100) * 10000;
}

// Push an event into the ring buffer
void evdev_push_event(evdev_device_t *dev, uint16_t type, uint16_t code,
                      int32_t value) {
  uint64_t flags;
  spinlock_acquire_save(&dev->lock, &flags);
  uint32_t next = (dev->head + 1) % EVDEV_RING_SIZE;
  if (next == dev->tail) {
    // Ring full — drop oldest event
    dev->tail = (dev->tail + 1) % EVDEV_RING_SIZE;
  }

  struct input_event *ev = &dev->ring[dev->head];
  evdev_timestamp(&ev->time_sec, &ev->time_usec);
  ev->type = type;
  ev->code = code;
  ev->value = value;
  dev->head = next;
  spinlock_release_restore(&dev->lock, flags);

  // Wake any threads blocked on read() or poll()
  wait_queue_wake_all(&dev->wait);

  // Notify epoll
  if (dev->vfs_node) {
    epoll_notify_event(dev->vfs_node, POLLIN);
  }
}

bool evdev_report_key(evdev_device_t *dev, uint16_t code, bool pressed) {
  if (!dev || code > KEY_MAX_EV)
    return false;

  uint64_t flags;
  spinlock_acquire_save(&dev->lock, &flags);
  uint8_t mask = (uint8_t)(1u << (code & 7));
  uint8_t *slot = &dev->key_state[code >> 3];
  bool was_pressed = (*slot & mask) != 0;

  if (!pressed && !was_pressed) {
    spinlock_release_restore(&dev->lock, flags);
    return false;
  }
  if (pressed)
    *slot |= mask;
  else
    *slot &= (uint8_t)~mask;
  spinlock_release_restore(&dev->lock, flags);

  evdev_push_event(dev, EV_KEY, code,
                   pressed ? (was_pressed ? 2 : 1) : 0);

  /* Sysrq-style hang report on demand (Right-Ctrl three times).  Called after the device
   * lock is dropped so a dump never runs holding it, and from the same path
   * both PS/2 and USB keyboards arrive through. */
  lockdiag_keyboard_event(code, pressed);
  return true;
}

void evdev_sync(evdev_device_t *dev) {
  if (dev)
    evdev_push_event(dev, EV_SYN, SYN_REPORT, 0);
}

// VFS read callback
// Returns one or more struct input_event records.
// If the ring is empty, blocks until events arrive.
static uint32_t evdev_vfs_read(struct vfs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  (void)offset;
  evdev_device_t *dev = (evdev_device_t *)node->device;
  struct thread *t = sched_get_current();
  if (!dev || !t)
    return 0;

  if (size < sizeof(struct input_event))
    return 0;

  // Heap-allocate wait queue entry to persist across context switches
  wait_queue_entry_t *entry = kmalloc(sizeof(wait_queue_entry_t));
  if (!entry)
    return 0;
  entry->thread = t;
  entry->next = NULL;

  while (1) {
    spinlock_acquire(&dev->lock);
    if (dev->head != dev->tail) {
      // Data available
      uint32_t copied = 0;
      uint32_t max_events = size / sizeof(struct input_event);
      while (copied < max_events && dev->tail != dev->head) {
        memcpy(buffer + copied * sizeof(struct input_event),
               &dev->ring[dev->tail], sizeof(struct input_event));
        dev->tail = (dev->tail + 1) % EVDEV_RING_SIZE;
        copied++;
      }
      spinlock_release(&dev->lock);
      kfree(entry);
      return copied * sizeof(struct input_event);
    }

    // No data available
    if (node->flags & FS_NONBLOCK) {
      spinlock_release(&dev->lock);
      kfree(entry);
      return (uint32_t)-11; // -EAGAIN
    }

    // No data, must block.
    wait_queue_add(&dev->wait, entry);
    t->state = THREAD_BLOCKED;

    // Check one last time while state is BLOCKED and we still hold the lock.
    // If an event comes now, it's either already here (seen by this check)
    // or it's waiting for the lock and will see us as BLOCKED.
    if (dev->head != dev->tail) {
      t->state = THREAD_RUNNING;
      spinlock_release(&dev->lock);
    } else {
      spinlock_release(&dev->lock);
      sched_yield();
    }

    // After waking up, remove from queue and try again
    wait_queue_remove(&dev->wait, entry);
    t->state = THREAD_RUNNING;
  }
}

// VFS poll callback
static int evdev_vfs_poll(struct vfs_node *node, int events) {
  evdev_device_t *dev = (evdev_device_t *)node->device;
  if (!dev)
    return 0;

  spinlock_acquire(&dev->lock);
  int revents = 0;
  if ((events & (POLLIN | POLLRDNORM)) && dev->head != dev->tail)
    revents |= (POLLIN | POLLRDNORM);
  spinlock_release(&dev->lock);
  return revents;
}

// Helper: set a bit in a bitmask array
static void set_bit(uint8_t *mask, int bit) {
  mask[bit / 8] |= (1 << (bit % 8));
}

// VFS ioctl callback
static int evdev_vfs_ioctl(struct vfs_node *node, uint32_t request,
                           uint64_t arg) {
  evdev_device_t *dev = (evdev_device_t *)node->device;
  if (!dev)
    return -1;

  // Decode the ioctl command
  // EVIOCGVERSION = 0x80044501
  if (request == 0x80044501) {
    uint32_t *version = (uint32_t *)arg;
    if (!version)
      return -14;
    *version = 0x010001; // Linux input driver version 1.0.1
    return 0;
  }

  // EVIOCGID = 0x80084502
  if (request == 0x80084502) {
    struct input_id *id = (struct input_id *)arg;
    if (!id)
      return -14;
    *id = dev->id;
    return 0;
  }

  // EVIOCGRAB = 0x40044590, EVIOCREVOKE = 0x40044591, EVIOCSCLOCKID = 0x400445A0
  if (request == 0x40044590 || request == 0x40044591 || request == 0x400445A0) {
    // Accept grab/ungrab/revoke/clockid but do nothing (single-user OS)
    return 0;
  }

  // EVIOCGNAME(len) — command byte 0x06 in the 0x45xx range
  // Format: 0x8000_4506 | (len << 16)
  if ((request & 0xC000FFFF) == 0x80004506) {
    char *buf = (char *)arg;
    if (!buf)
      return -14;
    uint32_t len = (request >> 16) & 0x3FFF;
    uint32_t name_len = strlen(dev->name);
    if (name_len >= len)
      name_len = len - 1;
    memcpy(buf, dev->name, name_len);
    buf[name_len] = '\0';
    return (int)name_len;
  }

  // EVIOCGPHYS(len)
  if ((request & 0xC000FFFF) == 0x80004507) {
    char *buf = (char *)arg;
    if (!buf)
      return -14;
    uint32_t len = (request >> 16) & 0x3FFF;
    uint32_t phys_len = strlen(dev->phys);
    if (phys_len >= len)
      phys_len = len - 1;
    memcpy(buf, dev->phys, phys_len);
    buf[phys_len] = '\0';
    return (int)phys_len;
  }

  // EVIOCGUNIQ(len)
  if ((request & 0xC000FFFF) == 0x80004508) {
    char *buf = (char *)arg;
    if (!buf)
      return -14;
    uint32_t len = (request >> 16) & 0x3FFF;
    if (len > 0)
      buf[0] = '\0';
    return 0;
  }

  // EVIOCGPROP(len)
  if ((request & 0xC000FFFF) == 0x80004509) {
    uint8_t *buf = (uint8_t *)arg;
    if (!buf)
      return -14;
    uint32_t len = (request >> 16) & 0x3FFF;
    memset(buf, 0, len);
    return 0;
  }

  // EVIOCGKEY(len) = 0x80xx4518
  // EVIOCGLED(len) = 0x80xx4519
  // EVIOCGSW(len)  = 0x80xx451b
  if ((request & 0xC000FFFF) == 0x80004518 ||
      (request & 0xC000FFFF) == 0x80004519 ||
      (request & 0xC000FFFF) == 0x8000451b) {
    uint8_t *buf = (uint8_t *)arg;
    if (!buf)
      return -14;
    uint32_t len = (request >> 16) & 0x3FFF;
    memset(buf, 0, len);
    if ((request & 0xC000FFFF) == 0x80004518) {
      uint32_t state_len = sizeof(dev->key_state);
      if (state_len > len)
        state_len = len;
      spinlock_acquire(&dev->lock);
      memcpy(buf, dev->key_state, state_len);
      spinlock_release(&dev->lock);
    }
    return 0;
  }
  
  // EVIOCGABS(abs) = 0x80184540 + abs
  if ((request & 0xC000FFC0) == 0x80004540) {
    struct input_absinfo *abs = (struct input_absinfo *)arg;
    if (!abs) return -14;
    memset(abs, 0, sizeof(struct input_absinfo));
    // For mouse, we don't really have absolute axes, but let's report something
    // reasonable if asked for ABS_X/ABS_Y
    uint8_t axis = request & 0x3F;
    if (axis == ABS_X || axis == ABS_Y) {
        abs->minimum = 0;
        abs->maximum = 32767;
        abs->resolution = 1;
    }
    return 0;
  }

  // EVIOCGREP = 0x80084503, EVIOCSREP = 0x40084503
  if (request == 0x80084503 || request == 0x40084503) {
      if (request == 0x80084503) {
          uint32_t *rep = (uint32_t *)arg;
          if (rep) {
              rep[0] = 250; // delay
              rep[1] = 33;  // period
          }
      }
      return 0;
  }

  // EVIOCGBIT(ev, len) — 0x80004520 | (ev << 8) | (len << 16)
  // Returns which event codes are supported for a given event type.
  if ((request & 0xC00000FF) == 0x80000020 && ((request >> 8) & 0xFF) >= 0x45) {
    // Reparse: the low 16 bits are 0x45EE where EE = event type field
    // Actually the encoding for EVIOCGBIT is complex. Let's match directly.
  }

  // Simpler approach: match the fixed bottom byte pattern
  // EVIOCGBIT(0, len)  = 0x80xx4520  — supported event types
  // EVIOCGBIT(1, len)  = 0x80xx4521  — supported KEY codes
  // EVIOCGBIT(2, len)  = 0x80xx4522  — supported REL codes
  // EVIOCGBIT(3, len)  = 0x80xx4523  — supported ABS codes
  uint32_t cmd_base = request & 0xC000FFFF;
  if (cmd_base >= 0x80004520 && cmd_base <= 0x80004540) {
    uint8_t *buf = (uint8_t *)arg;
    if (!buf)
      return -14;
    uint32_t len = (request >> 16) & 0x3FFF;
    uint32_t ev_type = (request & 0xFF) - 0x20; // 0x20 offsets from 0x4520

    // Zero-fill the output
    memset(buf, 0, len);

    if (ev_type == 0) {
      // EV_SYN bitmask (which event types this device supports)
      set_bit(buf, EV_SYN);
      set_bit(buf, EV_KEY);
      if (dev->type == EVDEV_MOUSE)
        set_bit(buf, EV_REL);
      return 0;
    }

    if (ev_type == EV_KEY) {
      // Supported key/button codes
      if (dev->type == EVDEV_KEYBOARD) {
        // Report all standard keys
        for (int i = 0; i < 256; i++) {
          if (i / 8 < (int)len)
            set_bit(buf, i);
        }
      } else if (dev->type == EVDEV_MOUSE) {
        // BTN_LEFT (0x110) / BTN_MOUSE = 272
        if (272 / 8 < (int)len)
          set_bit(buf, 272);
        if (273 / 8 < (int)len)
          set_bit(buf, 273);
        if (274 / 8 < (int)len)
          set_bit(buf, 274);
      }
      return 0;
    }

    if (ev_type == EV_REL) {
      if (dev->type == EVDEV_MOUSE) {
        set_bit(buf, REL_X);
        set_bit(buf, REL_Y);
      }
      return 0;
    }

    if (ev_type == EV_ABS) {
      if (dev->type == EVDEV_MOUSE) {
        set_bit(buf, ABS_X);
        set_bit(buf, ABS_Y);
      }
      return 0;
    }

    // Other event types: return zero-filled
    return 0;
  }

  // EVIOCGABS(axis) = 0x8018_4540 | axis
  if ((request & 0xC0FFFF00) == 0x80184500 && ((request & 0xFF) >= 0x40)) {
    uint32_t abs_axis = (request & 0xFF) - 0x40;
    struct input_absinfo *abs = (struct input_absinfo *)arg;
    if (!abs)
      return -14;

    memset(abs, 0, sizeof(*abs));
    if (dev->type == EVDEV_MOUSE) {
      if (abs_axis == ABS_X) {
        abs->minimum = 0;
        abs->maximum = (int32_t)fb_get_width() - 1;
        abs->value = 0;
        return 0;
      }
      if (abs_axis == ABS_Y) {
        abs->minimum = 0;
        abs->maximum = (int32_t)fb_get_height() - 1;
        abs->value = 0;
        return 0;
      }
    }
    return 0;
  }

  // FIONREAD (0x541B) — how many bytes are readable without blocking.
  // Return the number of complete input_event structs in the queue.
  if (request == 0x541B) {
    int *count = (int *)arg;
    if (!count)
      return -14;
    spinlock_acquire(&dev->lock);
    uint32_t avail = (dev->tail - dev->head + EVDEV_RING_SIZE) % EVDEV_RING_SIZE;
    *count = (int)(avail * sizeof(struct input_event));
    spinlock_release(&dev->lock);
    return 0;
  }

  // Silently reject TTY/serial/VT ioctls that programs probe on every fd.
  // These are always invalid on an evdev node; returning ENOTTY (-25) is
  // correct and the caller is expected to handle it gracefully.
  // Ranges:
  //   0x5400–0x54FF  TIOC* / termios ioctls  (e.g. TIOCGETD=0x540B, TCGETS=0x5401)
  //   0x5600–0x56FF  TIOCLINUX family
  //   0x4B00–0x4BFF  KIOCSOUND / keyboard console ioctls (KDGKBTYPE etc.)
  //   0x5900–0x59FF  HDIO (hard disk — opened on wrong fd)
  //   0x0000–0x00FF  old tty ioctl range
  uint8_t ioctl_type = (request >> 8) & 0xFF;
  if (ioctl_type == 0x54 || ioctl_type == 0x56 || ioctl_type == 0x4B ||
      ioctl_type == 0x59 || (request & 0xFFFFFF00) == 0) {
    // Known-harmless: don't log, just return ENOTTY so the caller can handle it
    return -25; // ENOTTY
  }

  // Only log ioctls that are in the EVIOC range (0x45xx) but unhandled —
  // those represent missing evdev feature coverage worth knowing about.
  if (((request & 0xFF) >= 0x00 && (request & 0xFF00) == 0x4500) ||
      ((request >> 8 & 0xFF) == 0x45)) {
    klog_puts("[EVDEV] unknown ioctl 0x");
    klog_hex32(request);
    klog_puts("\n");
  }
  return -25; // ENOTTY
}

// PS/2 scancode → Linux evdev keycode translation
// For the base range (scancode 0x00–0x58), the PS/2 set-1 scancode equals
// the Linux evdev keycode. Extended scancodes (0xE0 prefix) need a lookup.
static const uint8_t extended_scancode_to_keycode[] = {
    // Index = PS/2 scancode (after E0 prefix), value = Linux keycode
    // We only populate the ones we care about; rest are 0.
    [0x1C] = KEY_KPENTER_EV, [0x1D] = KEY_RIGHTCTRL, [0x35] = KEY_KPSLASH_EV,
    [0x38] = KEY_RIGHTALT,   [0x47] = KEY_HOME_EV,   [0x48] = KEY_UP_EV,
    [0x49] = KEY_PAGEUP_EV,  [0x4B] = KEY_LEFT_EV,   [0x4D] = KEY_RIGHT_EV,
    [0x4F] = KEY_END_EV,     [0x50] = KEY_DOWN_EV,   [0x51] = KEY_PAGEDOWN_EV,
    [0x52] = KEY_INSERT_EV,  [0x53] = KEY_DELETE_EV,
};

// Convert a PS/2 scancode to a Linux evdev keycode.
// Returns 0 if unmapped.
uint16_t evdev_ps2_to_keycode(uint8_t scancode, bool is_extended) {
  if (is_extended) {
    if (scancode < sizeof(extended_scancode_to_keycode))
      return extended_scancode_to_keycode[scancode];
    return 0;
  }
  // Base scancodes 1-88 map directly to Linux keycodes 1-88
  if (scancode >= 1 && scancode <= 88)
    return scancode;
  return 0;
}

// Create a VFS node for an evdev device
static void evdev_create_node(evdev_device_t *dev, const char *node_name, vfs_node_t *input_dir) {
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return;

  vfs_node_init(node);
  strcpy(node->name, node_name);
  node->flags = FS_CHARDEV | FS_PERSISTENT | FS_NONBLOCK;
  node->mask = 0666;
  node->device = dev;

  // Set device number (major 13, minor 64+) for libinput/seatd discovery
  if (strcmp(node_name, "event0") == 0) {
    node->inode = (13 << 8) | 64;
  } else if (strcmp(node_name, "event1") == 0) {
    node->inode = (13 << 8) | 65;
  }
  node->read = evdev_vfs_read;
  node->poll = evdev_vfs_poll;
  node->ioctl = evdev_vfs_ioctl;
  node->wait_queue = &dev->wait;

  dev->vfs_node = node;

  // 1. Register in the framebuffer device registry for syscall bypass
  char reg_name[64];
  strcpy(reg_name, "input/");
  strcat(reg_name, node_name);
  fb_register_device_node(reg_name, node);
  fb_register_device_node(node_name, node);

  // 2. Mount in the real VFS tree
  if (input_dir) {
    ramfs_mount_node(input_dir, node);
  }

  // If this is the mouse (event1), add standard aliases for X11
  if (strcmp(node_name, "event1") == 0) {
    fb_register_device_node("input/mice", node);
    fb_register_device_node("psaux", node);
    fb_register_device_node("mouse", node);
    
    if (input_dir) {
       vfs_node_t *mice = kmalloc(sizeof(vfs_node_t));
       if (mice) {
         vfs_node_init(mice);
         // Do not memcpy - it would break the list heads initialized above.
         // Manually copy the necessary fields for this alias.
         strcpy(mice->name, "mice");
         mice->flags = node->flags;
         mice->mask = node->mask;
         mice->device = node->device;
         mice->read = node->read;
         mice->poll = node->poll;
         mice->ioctl = node->ioctl;
         mice->wait_queue = node->wait_queue;
         mice->inode = node->inode;
         ramfs_mount_node(input_dir, mice);
       }
    }
  }
}

// Initialization
void evdev_init(void) {
  // Ensure /dev/input exist
  vfs_node_t *input_dir = NULL;
  vfs_node_t *dev_dir = vfs_resolve_path("/dev");
  if (dev_dir) {
    if (dev_dir->mkdir) {
        dev_dir->mkdir(dev_dir, "input", 0755);
    }
    input_dir = vfs_resolve_path("/dev/input");
  }

  // Keyboard device (event0)
  memset(&kbd_evdev, 0, sizeof(kbd_evdev));
  kbd_evdev.type = EVDEV_KEYBOARD;
  strcpy(kbd_evdev.name, "AT Translated Set 2 keyboard");
  strcpy(kbd_evdev.phys, "isa0060/serio0/input0");
  kbd_evdev.id.bustype = BUS_I8042;
  kbd_evdev.id.vendor = 0x0001;
  kbd_evdev.id.product = 0x0001;
  kbd_evdev.id.version = 0xAB41;
  wait_queue_init(&kbd_evdev.wait);
  spinlock_init(&kbd_evdev.lock);

  evdev_create_node(&kbd_evdev, "event0", input_dir);

  // Mouse device (event1)
  memset(&mouse_evdev, 0, sizeof(mouse_evdev));
  mouse_evdev.type = EVDEV_MOUSE;
  strcpy(mouse_evdev.name, "ImExPS/2 Generic Explorer Mouse");
  strcpy(mouse_evdev.phys, "isa0060/serio1/input0");
  mouse_evdev.id.bustype = BUS_I8042;
  mouse_evdev.id.vendor = 0x0002;
  mouse_evdev.id.product = 0x0005;
  mouse_evdev.id.version = 0x0000;
  wait_queue_init(&mouse_evdev.wait);
  spinlock_init(&mouse_evdev.lock);

  evdev_create_node(&mouse_evdev, "event1", input_dir);

  evdev_ready = true;

  klog_puts(
      "[OK] evdev input subsystem initialized (event0=kbd, event1=mouse)\n");
}
