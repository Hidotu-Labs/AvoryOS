/*
 * UHCI (Universal Host Controller Interface) — USB 1.x Driver
 *
 * Phase 1: PCI Discovery & Controller Skeleton
 *
 * This phase discovers all UHCI controllers on the PCI bus, extracts their
 * I/O base addresses and IRQ lines, silences the hardware (stops the
 * schedule, disables interrupts), and provides a self-test that verifies
 * register access and enumerates root hub port status.
 */

#include "uhci.h"
#include "../../console/console.h"
#include "../../console/klog.h"
#include "../../cpu/irq.h"
#include "../../io/io.h"
#include "../../mm/dma_alloc.h"
#include "../pci/pci.h"
#include "usb.h"
#include "usb_kbd.h"
#include "usb_mouse.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Private state

static struct uhci_controller controllers[UHCI_MAX_CONTROLLERS];
static int controller_count = 0;

// Helpers

static void print_hex16(uint16_t val) {
  const char *hex = "0123456789ABCDEF";
  console_putchar(hex[(val >> 12) & 0xF]);
  console_putchar(hex[(val >> 8) & 0xF]);
  console_putchar(hex[(val >> 4) & 0xF]);
  console_putchar(hex[val & 0xF]);
}

static void print_hex32(uint32_t val) {
  print_hex16((uint16_t)(val >> 16));
  print_hex16((uint16_t)(val & 0xFFFF));
}

static void print_uint32(uint32_t num) {
  if (num == 0) {
    console_putchar('0');
    return;
  }
  char buf[10];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0)
    console_putchar(buf[--i]);
}

// UHCI register I/O wrappers

static inline uint16_t uhci_read16(struct uhci_controller *hc, uint16_t reg) {
  return inw(hc->io_base + reg);
}

static inline void uhci_write16(struct uhci_controller *hc, uint16_t reg,
                                uint16_t val) {
  outw(hc->io_base + reg, val);
}

static inline uint32_t uhci_read32(struct uhci_controller *hc, uint16_t reg) {
  return inl(hc->io_base + reg);
}

static inline void uhci_write32(struct uhci_controller *hc, uint16_t reg,
                                uint32_t val) {
  outl(hc->io_base + reg, val);
}

static inline uint8_t uhci_read8(struct uhci_controller *hc, uint16_t reg) {
  return inb(hc->io_base + reg);
}

static inline void uhci_write8(struct uhci_controller *hc, uint16_t reg,
                               uint8_t val) {
  outb(hc->io_base + reg, val);
}

// IRQ Handler

static void uhci_irq_handler(struct registers *regs) {
  (void)regs;
  bool handled = false;

  for (int i = 0; i < controller_count; i++) {
    struct uhci_controller *hc = &controllers[i];
    if (!hc->present)
      continue;

    uint16_t sts = uhci_read16(hc, UHCI_REG_USBSTS);
    if (sts == 0)
      continue;

    // Acknowledge the interrupt by writing 1 to the status bits
    uhci_write16(hc, UHCI_REG_USBSTS, sts);
    hc->interrupts++;
    handled = true;

    if (sts & (UHCI_STS_HSE | UHCI_STS_HCPE)) {
      klog_puts("[UHCI] Fatal Status: 0x");
      klog_hex32(sts);
      klog_puts("\n");
    }

    if (sts & UHCI_STS_USBINT) {
      usb_kbd_poll();
      usb_mouse_poll();
    }
  }

  if (!handled) {
    // Shared interrupt handled elsewhere or spurious
  }
}

// Controller silence
// Ensure the controller is stopped and won't fire spurious IRQs before we're
// ready. This is critical during early boot when IRQ routing may not be set up.

static void uhci_silence(struct uhci_controller *hc) {
  // 0. Disable PCI INTx at the PCI config level.
  uint32_t pci_cmd =
      pci_config_read32(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04);
  pci_cmd |= (1 << 10); // PCI Command: Interrupt Disable
  pci_config_write32(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04, pci_cmd);

  // 1. More aggressive reset: Toggle Global Reset and Host Controller Reset
  //    This ensures the hardware is in a known clean state.
  uhci_write16(hc, UHCI_REG_USBCMD, UHCI_CMD_GRESET);
  for (int i = 0; i < 50000; i++)
    io_wait(); // Wait 50ms for reset
  uhci_write16(hc, UHCI_REG_USBCMD, 0x0000);
  for (int i = 0; i < 20000; i++)
    io_wait(); // 20ms recovery

  uhci_write16(hc, UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
  for (int i = 0; i < 1000; i++) {
    if (!(uhci_read16(hc, UHCI_REG_USBCMD) & UHCI_CMD_HCRESET))
      break;
    io_wait();
  }

  // 2. Disable all UHCI interrupts
  uhci_write16(hc, UHCI_REG_USBINTR, 0x0000);

  // 3. Clear any pending status bits (write-1-to-clear)
  uhci_write16(hc, UHCI_REG_USBSTS, 0xFFFF);

  // 4. Final confirmation of halt
  for (int i = 0; i < 1000; i++) {
    uint16_t s = uhci_read16(hc, UHCI_REG_USBSTS);
    uint16_t c = uhci_read16(hc, UHCI_REG_USBCMD);
    if (s & UHCI_STS_HCH)
      return;
    if (!(c & UHCI_CMD_RS) && s == 0x0000)
      return;
    io_wait();
  }

  klog_puts("[UHCI] Warning: controller did not halt cleanly\n");
}

void uhci_run(struct uhci_controller *hc) {
  uint16_t cmd = uhci_read16(hc, UHCI_REG_USBCMD);
  cmd |= UHCI_CMD_RS | UHCI_CMD_CF;
  uhci_write16(hc, UHCI_REG_USBCMD, cmd);
}

static void uhci_init_pools(struct uhci_controller *hc) {
  uint64_t fl_phys, td_phys, qh_phys, buf_phys;

  hc->frame_list = (uint32_t *)dma_alloc_pages(1, &fl_phys);
  hc->frame_list_phys = (uint32_t)fl_phys;

  hc->td_pool = (struct uhci_td *)dma_alloc_pages(1, &td_phys);
  hc->td_pool_phys = (uint32_t)td_phys;

  hc->qh_pool = (struct uhci_qh *)dma_alloc_pages(1, &qh_phys);
  hc->qh_pool_phys = (uint32_t)qh_phys;

  hc->transfer_buffer = dma_alloc_pages(1, &buf_phys);
  hc->transfer_buffer_phys = (uint32_t)buf_phys;

  // Zero the pools
  for (int i = 0; i < 1024; i++)
    hc->frame_list[i] = 1; // Terminate
  for (int i = 0; i < 256; i++) {
    hc->td_pool[i].link = 1;
    hc->td_pool[i].status = 0;
    hc->qh_pool[i].head = 1;
    hc->qh_pool[i].element = 1;
  }

  // Point all frames to the same first QH
  for (int i = 0; i < 1024; i++) {
    hc->frame_list[i] = hc->qh_pool_phys | TD_LINK_QH;
  }
}

static struct uhci_td *uhci_alloc_td(struct uhci_controller *hc,
                                     uint32_t *phys) {
  for (int i = 0; i < 256; i++) {
    if (!(hc->td_pool[i].status & TD_STATUS_ACTIVE) &&
        (hc->td_pool[i].link & TD_LINK_TERMINATE)) {
      if (phys)
        *phys = hc->td_pool_phys + (i * sizeof(struct uhci_td));
      // Mark as allocated so the next call doesn't return the same slot
      hc->td_pool[i].link = 0;
      hc->td_pool[i].status = 0;
      hc->td_pool[i].token = 0;
      hc->td_pool[i].buffer = 0;
      return &hc->td_pool[i];
    }
  }
  return NULL;
}

int uhci_control_transfer(struct uhci_controller *hc, uint8_t addr,
                          struct usb_control_request *req, void *data,
                          uint16_t len, bool low_speed) {
  // 1. Copy request into DMA buffer
  struct usb_control_request *dma_req =
      (struct usb_control_request *)hc->transfer_buffer;
  *dma_req = *req;

  uint8_t *dma_data = (uint8_t *)hc->transfer_buffer + 64;
  if (data && len > 0 && !(req->request_type & 0x80)) {
    for (uint16_t i = 0; i < len; i++)
      dma_data[i] = ((uint8_t *)data)[i];
  }

  // 2. Allocate TDs
  uint32_t setup_phys, data_phys = 0, status_phys;
  struct uhci_td *setup_td = uhci_alloc_td(hc, &setup_phys);
  struct uhci_td *status_td = uhci_alloc_td(hc, &status_phys);
  struct uhci_td *data_td = NULL;

  if (len > 0)
    data_td = uhci_alloc_td(hc, &data_phys);

  if (!setup_td || !status_td || (len > 0 && !data_td))
    return -1;

  // 3. Build Setup TD
  //   MaxLen=7 (8 bytes), Data Toggle=0, Endpoint=0, Address=addr
  setup_td->status = TD_STATUS_ACTIVE | TD_STATUS_C_ERR;
  if (low_speed)
    setup_td->status |= TD_STATUS_LS;
  setup_td->token =
      (7 << 21) | (0 << 19) | (0 << 15) | (addr << 8) | TD_PID_SETUP;
  setup_td->buffer = hc->transfer_buffer_phys;

  // 4. Build Data TD (optional)
  if (data_td) {
    setup_td->link = data_phys | TD_LINK_VF;  // depth-first
    data_td->link = status_phys | TD_LINK_VF; // depth-first
    data_td->status = TD_STATUS_ACTIVE | TD_STATUS_C_ERR;
    if (low_speed)
      data_td->status |= TD_STATUS_LS;

    uint8_t pid = (req->request_type & 0x80) ? TD_PID_IN : TD_PID_OUT;
    data_td->token =
        ((uint32_t)(len - 1) << 21) | (1 << 19) | (0 << 15) | (addr << 8) | pid;
    data_td->buffer = hc->transfer_buffer_phys + 64;
  } else {
    setup_td->link = status_phys | TD_LINK_VF;
  }

  // 5. Build Status TD
  //   MaxLen=0x7FF (zero-length), Data Toggle=1, IOC=1
  uint8_t status_pid =
      (len == 0 || !(req->request_type & 0x80)) ? TD_PID_IN : TD_PID_OUT;
  status_td->link = TD_LINK_TERMINATE;
  status_td->status = TD_STATUS_ACTIVE | TD_STATUS_IOC | TD_STATUS_C_ERR;
  if (low_speed)
    status_td->status |= TD_STATUS_LS;
  status_td->token =
      (0x7FF << 21) | (1 << 19) | (0 << 15) | (addr << 8) | status_pid;
  status_td->buffer = 0;

  // 6. Link into schedule
  //   qh_pool[0] is already in every frame list slot.
  //   Point its element directly to our first TD.
  asm volatile("mfence" ::: "memory");
  hc->qh_pool[0].element = setup_phys;
  asm volatile("mfence" ::: "memory");

  // 7. Poll for completion
  bool timed_out = true;
  for (int i = 0; i < 2000000; i++) {
    if (!(status_td->status & TD_STATUS_ACTIVE)) {
      timed_out = false;
      break;
    }
    // Also check if setup or data TD halted (error)
    if (setup_td->status & TD_STATUS_HALTED) {
      timed_out = false;
      break;
    }
    if (data_td && (data_td->status & TD_STATUS_HALTED)) {
      timed_out = false;
      break;
    }
    for (int j = 0; j < 10; j++)
      io_wait();
  }

  // 8. Unlink from schedule
  hc->qh_pool[0].element = TD_LINK_TERMINATE;
  asm volatile("mfence" ::: "memory");

  // 9. Check result
  if (timed_out || (setup_td->status & TD_STATUS_HALTED) ||
      (data_td && (data_td->status & TD_STATUS_HALTED))) {
    klog_puts("[UHCI] Xfer fail. SETUP=0x");
    klog_hex32(setup_td->status);
    if (data_td) {
      klog_puts(" DATA=0x");
      klog_hex32(data_td->status);
    }
    klog_puts(" STS=0x");
    klog_hex32(status_td->status);
    klog_puts(" USBSTS=0x");
    klog_hex32(uhci_read16(hc, UHCI_REG_USBSTS));
    klog_puts("\n");
    // Return TDs to pool
    setup_td->link = TD_LINK_TERMINATE;
    setup_td->status = 0;
    status_td->link = TD_LINK_TERMINATE;
    status_td->status = 0;
    if (data_td) {
      data_td->link = TD_LINK_TERMINATE;
      data_td->status = 0;
    }
    return -2;
  }

  // 10. Copy received data
  if (data && len > 0 && (req->request_type & 0x80)) {
    for (uint16_t i = 0; i < len; i++)
      ((uint8_t *)data)[i] = dma_data[i];
  }

  // 11. Free TDs
  setup_td->link = TD_LINK_TERMINATE;
  setup_td->status = 0;
  status_td->link = TD_LINK_TERMINATE;
  status_td->status = 0;
  if (data_td) {
    data_td->link = TD_LINK_TERMINATE;
    data_td->status = 0;
  }

  return 0;
}

static int uhci_hcd_control_transfer(struct usb_hcd *hcd, uint8_t addr,
                                     struct usb_control_request *req,
                                     void *data, uint16_t len,
                                     enum usb_speed speed) {
  struct uhci_controller *hc = (struct uhci_controller *)hcd->priv;
  return uhci_control_transfer(hc, addr, req, data, len,
                               usb_speed_is_low(speed));
}

// Detect number of root-hub ports
// UHCI spec says ports start at offset 0x10 and each is 2 bytes.
// We probe up to 8 ports; a non-existent port reads as 0xFFFF or has
// reserved bits set that real ports never show.

static uint8_t uhci_detect_ports(struct uhci_controller *hc) {
  uint8_t count = 0;
  for (int i = 0; i < 8; i++) {
    uint16_t portsc = inw(hc->io_base + UHCI_REG_PORTSC1 + (i * 2));
    // Bit 7 is always read as 1 on real UHCI ports
    if (!(portsc & (1 << 7)))
      break;
    // Bits 13-15 must be 0 on a valid port
    if (portsc & 0xE000)
      break;
    count++;
  }
  return count ? count : 2; // Default to 2 if detection fails
}

void uhci_reset_port(struct uhci_controller *hc, uint8_t port) {
  uint16_t reg = UHCI_REG_PORTSC1 + (port * 2);

  // 1. Assert Port Reset
  uint16_t portsc = uhci_read16(hc, reg);
  portsc |= UHCI_PORT_PR;
  uhci_write16(hc, reg, portsc);

  // 2. Wait for reset to complete (USB spec: 10ms-50ms)
  for (int i = 0; i < 50000; i++)
    io_wait();

  // 3. Clear Port Reset
  portsc = uhci_read16(hc, reg);
  portsc &= ~UHCI_PORT_PR;
  uhci_write16(hc, reg, portsc);

  // 4. Recovery time (USB spec: 10ms)
  for (int i = 0; i < 20000; i++)
    io_wait();

  // 5. Wait for Port Enable (hardware enables it if successful)
  // If hardware doesn't enable it, WE try to enable it (older HCs need this)
  for (int i = 0; i < 1000; i++) {
    portsc = uhci_read16(hc, reg);
    if (portsc & UHCI_PORT_PE)
      break;

    if (i == 500) {
      // Halfway through, try to force it
      portsc |= UHCI_PORT_PE;
      uhci_write16(hc, reg, portsc);
    }
    io_wait();
  }
}

void uhci_probe_ports(struct uhci_controller *hc) {
  for (uint8_t i = 0; i < hc->num_ports; i++) {
    uint16_t reg = UHCI_REG_PORTSC1 + (i * 2);
    uint16_t portsc = uhci_read16(hc, reg);

    // Initial check: if connected but not enabled, try a reset.
    // Also check for Connect Status Change (CSC).
    if ((portsc & UHCI_PORT_CSC) ||
        ((portsc & UHCI_PORT_CCS) && !(portsc & UHCI_PORT_PE))) {
      klog_puts("[UHCI] Connection activity on port ");
      klog_uint64(i + 1);
      klog_puts("\n");

      // Clear the CSC bit (it's write-1-to-clear)
      uhci_write16(hc, reg, portsc | UHCI_PORT_CSC);

      if (portsc & UHCI_PORT_CCS) {
        uhci_reset_port(hc, i);

        // Re-read status to check speed
        portsc = uhci_read16(hc, reg);
        bool low_speed = (portsc & UHCI_PORT_LSDA) != 0;

        usb_device_discovered(&hc->hcd, i,
                              low_speed ? USB_SPEED_LOW : USB_SPEED_FULL);
      }
    }
  }
}

// PCI Discovery

static bool uhci_probe_pci_device(struct pci_device *pci) {
  if (controller_count >= UHCI_MAX_CONTROLLERS)
    return false;

  // UHCI uses I/O space — find the first BAR with bit 0 set (I/O indicator)
  uint16_t io_base = 0;
  for (int i = 0; i < 6; i++) {
    if ((pci->bar[i] & 0x01) && (pci->bar[i] & ~0x03) != 0) {
      io_base = (uint16_t)(pci->bar[i] & ~0x03);
      break;
    }
  }

  if (io_base == 0) {
    klog_puts("[UHCI] No I/O BAR found for controller at ");
    klog_hex32(pci->bus);
    klog_puts(":");
    klog_hex32(pci->slot);
    klog_puts("\n");
    return false;
  }

  struct uhci_controller *hc = &controllers[controller_count];
  hc->io_base = io_base;
  hc->irq_line = pci->irq_line;
  hc->pci_bus = pci->bus;
  hc->pci_slot = pci->slot;
  hc->pci_func = pci->func;
  hc->vendor_id = pci->vendor_id;
  hc->device_id = pci->device_id;
  hc->present = true;

  // Silence the controller before anything else
  outw(hc->io_base + UHCI_REG_USBINTR, 0x0000);
  outw(hc->io_base + UHCI_REG_USBCMD, 0x0000);
  outw(hc->io_base + UHCI_REG_USBSTS, 0x003F); // Clear all status bits

  // Initialize transfer pools and schedule
  uhci_init_pools(hc);

  // Detect how many root hub ports this controller has
  hc->num_ports = uhci_detect_ports(hc);

  // Register IRQ Handler
  if (!hc->irq_registered) {
    if (irq_install_handler(hc->irq_line, uhci_irq_handler, 0x000F)) {
      hc->irq_registered = true;
    }
  }

  // Initialize generic HCD interface
  hc->hcd.priv = hc;
  hc->hcd.name = "uhci";
  hc->hcd.control_transfer = uhci_hcd_control_transfer;
  hc->hcd.address_device = NULL;
  hc->hcd.device_removed = NULL;

  // Re-enable PCI bus mastering and IO
  pci_config_write32(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04, 0x07);

  // Set the frame list base address
  uhci_write32(hc, UHCI_REG_FLBASEADD, hc->frame_list_phys);
  uhci_write16(hc, UHCI_REG_FRNUM, 0x0000);
  uhci_write16(hc, UHCI_REG_USBINTR, 0x000F); // Enable all interrupts

  // Start the controller
  uhci_run(hc);

  controller_count++;
  return true;
}

// Public API

void uhci_init(void) {
  controller_count = 0;
  console_puts("[INFO] Searching for UHCI (USB 1.x) controllers...\n");

  // First pass: silence ALL USB controllers (UHCI, EHCI, xHCI)
  // Q35's ICH9 has EHCI (prog_if 0x20) in addition to UHCI.  Any of these
  // can assert level-triggered interrupts on shared IRQ lines, starving
  // other devices.  Disable PCI INTx for every USB controller we find.
  uint32_t pci_count = pci_get_device_count();
  for (uint32_t i = 0; i < pci_count; i++) {
    struct pci_device *dev = pci_get_device(i);
    if (!dev)
      continue;

    if (dev->class_code == PCI_CLASS_SERIAL_BUS &&
        dev->subclass == PCI_SUBCLASS_USB) {
      // Disable PCI INTx for this USB controller
      uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
      cmd |= (1 << 10); // PCI Command: Interrupt Disable

      // For NON-UHCI controllers (like EHCI/xHCI), strictly disable them
      // by pulling the plug on Master, Memory, and I/O access.
      // We don't have drivers for them yet, so they shouldn't be active.
      if (dev->prog_if != PCI_PROGIF_UHCI && dev->prog_if != PCI_PROGIF_OHCI) {
        // Skip EHCI controllers — they are managed by our ehci.c driver
        if (dev->prog_if == 0x20)
          continue;
        // xHCI is initialized before the companion-controller pass.
        if (dev->prog_if == 0x30)
          continue;

        cmd &= ~(0x07); // Clear Master, Memory, I/O
        klog_puts("[USB] Electrically disabled non-UHCI controller at PCI ");
        klog_hex32(dev->bus);
        klog_puts(":");
        klog_hex32(dev->slot);
        klog_puts(".");
        klog_hex32(dev->func);
        klog_puts("\n");
      }

      pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

      // For UHCI controllers, disable Legacy Support (USB LEGSUP at 0xC0)
      // This is crucial to stop BIOS from interfering or firing SMIs.
      if (dev->prog_if == PCI_PROGIF_UHCI) {
        pci_config_write16(dev->bus, dev->slot, dev->func, 0xC0, 0x8F00);
      }
    }
  }

  // Second pass: probe UHCI-specific controllers
  for (uint32_t i = 0; i < pci_count; i++) {
    struct pci_device *dev = pci_get_device(i);
    if (!dev)
      continue;

    // Match: Serial Bus Controller (0x0C) → USB (0x03) → UHCI (0x00)
    if (dev->class_code == PCI_CLASS_SERIAL_BUS &&
        dev->subclass == PCI_SUBCLASS_USB && dev->prog_if == PCI_PROGIF_UHCI) {
      uhci_probe_pci_device(dev);
    }
  }

  if (controller_count == 0) {
    console_puts("[WARN] No UHCI controllers found.\n");
    return;
  }

  console_puts("[OK] UHCI: Found ");
  print_uint32(controller_count);
  console_puts(" controller(s):\n");

  usb_init();

  for (int i = 0; i < controller_count; i++) {
    struct uhci_controller *hc = &controllers[i];
    console_puts("     uhci");
    console_putchar('0' + i);
    console_puts(": PCI ");
    print_uint32(hc->pci_bus);
    console_putchar(':');
    print_uint32(hc->pci_slot);
    console_putchar('.');
    print_uint32(hc->pci_func);
    console_puts(" IO=0x");
    print_hex16(hc->io_base);
    console_puts(" IRQ=");
    print_uint32(hc->irq_line);
    console_puts(" ports=");
    print_uint32(hc->num_ports);
    console_putchar('\n');

    // Initial root hub probe
    uhci_probe_ports(hc);
  }
}

int uhci_get_controller_count(void) { return controller_count; }

struct uhci_controller *uhci_get_controller(int index) {
  if (index < 0 || index >= controller_count)
    return NULL;
  return &controllers[index];
}

// Self-Test
// Verifies that we can read/write UHCI registers and dumps diagnostic state.

void uhci_self_test(void) {
  console_puts("\n[TEST] UHCI Self-Test\n");
  console_puts("─────────────────────────────────────────\n");

  if (controller_count == 0) {
    console_puts("  SKIP: No UHCI controllers present.\n");
    console_puts("─────────────────────────────────────────\n\n");
    return;
  }

  int pass = 0;
  int fail = 0;

  for (int i = 0; i < controller_count; i++) {
    struct uhci_controller *hc = &controllers[i];
    console_puts("  Controller ");
    console_putchar('0' + i);
    console_puts(" (IO=0x");
    print_hex16(hc->io_base);
    console_puts("):\n");

    // Test 1: HC should be stopped after uhci_silence()
    //    Accept either HCH=1 (explicitly halted) or the "never started"
    //    state where USBCMD.RS=0 and USBSTS is clean (0x0000).
    uint16_t sts = uhci_read16(hc, UHCI_REG_USBSTS);
    uint16_t cmd_chk = uhci_read16(hc, UHCI_REG_USBCMD);
    // If it's the second pass, it SHOULD be running
    bool running = (cmd_chk & UHCI_CMD_RS);
    if (running) {
      console_puts("    [PASS] Controller is running (USBSTS=0x");
      print_hex16(sts);
      console_puts(")\n");
      pass++;
    } else {
      console_puts("    [FAIL] Controller NOT running (USBSTS=0x");
      print_hex16(sts);
      console_puts(")\n");
      fail++;
    }

    uint16_t cmd = uhci_read16(hc, UHCI_REG_USBCMD);
    if (cmd & UHCI_CMD_CF) {
      console_puts("    [PASS] USBCMD configure flag set (0x");
      print_hex16(cmd);
      console_puts(")\n");
      pass++;
    } else {
      console_puts("    [FAIL] USBCMD configure flag NOT set (0x");
      print_hex16(cmd);
      console_puts(")\n");
      fail++;
    }

    uint16_t intr = uhci_read16(hc, UHCI_REG_USBINTR);
    uint16_t expected_intr =
        UHCI_INTR_IOC | UHCI_INTR_TIMEOUT | UHCI_INTR_SP | UHCI_INTR_RESUME;
    if (intr == expected_intr) {
      console_puts("    [PASS] Interrupts enabled (USBINTR=0x");
      print_hex16(intr);
      console_puts(")\n");
      pass++;
    } else {
      console_puts("    [FAIL] Interrupt mask mismatch (USBINTR=0x");
      print_hex16(intr);
      console_puts(")\n");
      fail++;
    }

    // Test 4: SOF Modify register should default to 0x40 (64)
    uint8_t sof = uhci_read8(hc, UHCI_REG_SOFMOD);
    if (sof == 0x40) {
      console_puts("    [PASS] SOFMOD default correct (0x40)\n");
      pass++;
    } else {
      console_puts("    [WARN] SOFMOD non-standard (0x");
      const char *hex = "0123456789ABCDEF";
      console_putchar(hex[(sof >> 4) & 0xF]);
      console_putchar(hex[sof & 0xF]);
      console_puts(")\n");
      // Not a hard fail — some chips differ
      pass++;
    }

    // Test 5: Frame List Base Address register is writable
    uint32_t old_fl = uhci_read32(hc, UHCI_REG_FLBASEADD);
    uhci_write32(hc, UHCI_REG_FLBASEADD, 0xDEAD0000);
    uint32_t new_fl = uhci_read32(hc, UHCI_REG_FLBASEADD);
    uhci_write32(hc, UHCI_REG_FLBASEADD, old_fl); // restore
    // Bottom 12 bits are always 0 (4K alignment), so expect 0xDEAD0000
    if ((new_fl & 0xFFFFF000) == 0xDEAD0000) {
      console_puts("    [PASS] FLBASEADD register is writable\n");
      pass++;
    } else {
      console_puts("    [FAIL] FLBASEADD read-back mismatch (0x");
      print_hex32(new_fl);
      console_puts(")\n");
      fail++;
    }

    // Test 6: Dump root hub port status
    console_puts("    Port Status:\n");
    for (uint8_t p = 0; p < hc->num_ports; p++) {
      uint16_t portsc = uhci_read16(hc, UHCI_REG_PORTSC1 + (p * 2));
      console_puts("      Port ");
      console_putchar('1' + p);
      console_puts(": 0x");
      print_hex16(portsc);

      if (portsc & UHCI_PORT_CCS) {
        console_puts(" [CONNECTED");
        if (portsc & UHCI_PORT_LSDA)
          console_puts(", Low-Speed");
        else
          console_puts(", Full-Speed");
        console_putchar(']');
      } else {
        console_puts(" [EMPTY]");
      }

      if (portsc & UHCI_PORT_PE)
        console_puts(" ENABLED");

      console_putchar('\n');
    }

    // Test 7: Frame List allocation
    if (hc->frame_list && (hc->frame_list_phys & 0xFFF) == 0) {
      console_puts("    [PASS] Frame List allocated at 0x");
      print_hex32(hc->frame_list_phys);
      console_putchar('\n');
      pass++;
    } else {
      console_puts("    [FAIL] Frame List allocation/alignment error\n");
      fail++;
    }

    // Test 8: IRQ registration
    if (hc->irq_registered) {
      console_puts("    [PASS] IRQ handler registered (IRQ ");
      print_uint32(hc->irq_line);
      console_puts(")\n");
      pass++;
    } else {
      console_puts("    [FAIL] IRQ handler NOT registered\n");
      fail++;
    }
  }

  // Summary
  console_puts("─────────────────────────────────────────\n");
  console_puts("  Results: ");
  print_uint32(pass);
  console_puts(" passed, ");
  print_uint32(fail);
  console_puts(" failed\n");

  if (fail == 0) {
    console_puts("  ✓ UHCI Phase 2 PASSED\n");
  } else {
    console_puts("  ✗ UHCI Phase 2 FAILED\n");
  }
  console_puts("─────────────────────────────────────────\n\n");
}
