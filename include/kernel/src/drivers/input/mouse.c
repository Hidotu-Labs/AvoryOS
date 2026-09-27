#include "drivers/input/mouse.h"
#include "../../console/klog.h"
#include "../../cpu/irq.h"
#include "../../cpu/isr.h"
#include "../../fb/framebuffer.h"
#include "../../fs/vfs.h"
#include "../../io/io.h"
#include "../../lib/string.h"
#include "../../lock/spinlock.h"
#include "../../mm/heap.h"
#include "drivers/input/evdev.h"
#include <stdbool.h>
#include <stdint.h>

#define MOUSE_DATA_PORT 0x60
#define MOUSE_STATUS_PORT 0x64
#define MOUSE_COMMAND_PORT 0x64

#define MOUSE_ACK 0xFA

static mouse_state_t global_mouse_state = {0, 0, false, false, false};
static bool prev_left = false, prev_right = false, prev_middle = false;
static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];

static int mouse_wait(uint8_t type) {
  uint32_t timeout = 100000;
  uint8_t status;
  if (type == 0) {
    while (!((status = inb(MOUSE_STATUS_PORT)) & 1) && timeout--) {
      if (status == 0xFF)
        return -1;
      io_wait();
    }
  } else {
    while (((status = inb(MOUSE_STATUS_PORT)) & 2) && timeout--) {
      if (status == 0xFF)
        return -1;
      io_wait();
    }
  }
  if (timeout == 0) {
    klog_puts("[MOUSE] Warning: mouse_wait timeout!\n");
    return -2;
  }
  return 0;
}

static void mouse_write(uint8_t data) {
  mouse_wait(1);
  outb(MOUSE_COMMAND_PORT, 0xD4);
  mouse_wait(1);
  outb(MOUSE_DATA_PORT, data);
}

static uint8_t mouse_read(void) {
  mouse_wait(0);
  return inb(MOUSE_DATA_PORT);
}

static void mouse_callback(struct registers *regs) {
  (void)regs;

  // Read status to ensure it's actually mouse data
  uint8_t status = inb(MOUSE_STATUS_PORT);
  if (!(status & 1) || !(status & 0x20)) {
    return; // No data or not mouse data
  }

  uint8_t data = inb(MOUSE_DATA_PORT);

  switch (mouse_cycle) {
  case 0:
    mouse_packet[0] = data;
    // Byte 0 bit 3 must be 1 for a valid packet start
    if (data & 0x08) {
      mouse_cycle++;
    }
    break;
  case 1:
    mouse_packet[1] = data;
    mouse_cycle++;
    break;
  case 2:
    mouse_packet[2] = data;
    mouse_cycle = 0;

    // Decode buttons
    global_mouse_state.left_button = (mouse_packet[0] & 0x01) != 0;
    global_mouse_state.right_button = (mouse_packet[0] & 0x02) != 0;
    global_mouse_state.middle_button = (mouse_packet[0] & 0x04) != 0;

    // Decode deltas
    int32_t dx = (int32_t)mouse_packet[1];
    int32_t dy = (int32_t)mouse_packet[2];

    // Sign extension
    if (mouse_packet[0] & 0x10)
      dx -= 256;
    if (mouse_packet[0] & 0x20)
      dy -= 256;

    // PS/2 mouse Y axis is inverted (up is positive)
    // Screen Y axis: down is positive
    dy = -dy;

    // Update absolute coordinates
    global_mouse_state.x += dx;
    global_mouse_state.y += dy;

    // Clamp to screen
    uint32_t width = fb_get_width();
    uint32_t height = fb_get_height();

    if (global_mouse_state.x < 0)
      global_mouse_state.x = 0;
    if (global_mouse_state.y < 0)
      global_mouse_state.y = 0;
    if (width > 0 && global_mouse_state.x >= (int32_t)width)
      global_mouse_state.x = width - 1;
    if (height > 0 && global_mouse_state.y >= (int32_t)height)
      global_mouse_state.y = height - 1;

    // Push evdev events for X11
    evdev_device_t *mdev = evdev_get_mouse();
    if (mdev) {
      // Relative motion events
      if (dx != 0)
        evdev_push_event(mdev, EV_REL, REL_X, dx);
      if (dy != 0)
        evdev_push_event(mdev, EV_REL, REL_Y, dy);

      // Button state change events
      bool cur_left = global_mouse_state.left_button;
      bool cur_right = global_mouse_state.right_button;
      bool cur_middle = global_mouse_state.middle_button;

      bool changed = dx != 0 || dy != 0;
      if (cur_left != prev_left)
        changed |= evdev_report_key(mdev, BTN_LEFT, cur_left);
      if (cur_right != prev_right)
        changed |= evdev_report_key(mdev, BTN_RIGHT, cur_right);
      if (cur_middle != prev_middle)
        changed |= evdev_report_key(mdev, BTN_MIDDLE, cur_middle);

      prev_left = cur_left;
      prev_right = cur_right;
      prev_middle = cur_middle;

      // SYN_REPORT marks the end of this event batch
      if (changed)
        evdev_sync(mdev);
    }
    break;
  }
}

void mouse_init(void) {
  // Check if PS/2 controller exists at all
  if (inb(MOUSE_STATUS_PORT) == 0xFF) {
    klog_puts("[MOUSE] PS/2 controller not found or floating bus. Skipping.\n");
    return;
  }

  klog_puts("[INFO] Initializing PS/2 Mouse...\n");

  // Enable the auxiliary mouse device
  if (mouse_wait(1) != 0)
    return;
  outb(MOUSE_COMMAND_PORT, 0xA8);

  // Enable interrupts
  if (mouse_wait(1) != 0)
    return;
  outb(MOUSE_COMMAND_PORT, 0x20); // Get Compaq Status Byte
  if (mouse_wait(0) != 0)
    return;
  uint8_t status = inb(MOUSE_DATA_PORT) | 2; // Set IRQ12 bit
  status &= ~0x20;                           // Clear "disable mouse" bit

  mouse_wait(1);
  outb(MOUSE_COMMAND_PORT, 0x60); // Set Compaq Status Byte
  mouse_wait(1);
  outb(MOUSE_DATA_PORT, status);

  // Set default settings
  mouse_write(0xF6);
  mouse_read(); // ACK

  // Enable data reporting
  mouse_write(0xF4);
  mouse_read(); // ACK

  // Register interrupt handler
  irq_install_handler(12, mouse_callback, 0);

  klog_puts("[OK] PS/2 Mouse Initialized.\n");
}

mouse_state_t mouse_get_state(void) { return global_mouse_state; }

// VFS Integration

static uint32_t mouse_vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                               uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (size < sizeof(mouse_device_packet_t))
    return 0;

  mouse_device_packet_t packet;
  packet.x = global_mouse_state.x;
  packet.y = global_mouse_state.y;
  packet.buttons = 0;
  if (global_mouse_state.left_button)
    packet.buttons |= 1;
  if (global_mouse_state.right_button)
    packet.buttons |= 2;
  if (global_mouse_state.middle_button)
    packet.buttons |= 4;

  memcpy(buffer, &packet, sizeof(mouse_device_packet_t));
  return sizeof(mouse_device_packet_t);
}

void mouse_register_vfs(void) {
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return;

  vfs_node_init(node);
  strcpy(node->name, "mouse");
  node->flags = FS_CHARDEV;
  node->mask = 0666;
  node->read = mouse_vfs_read;

  fb_register_device_node("mouse", node);
}
