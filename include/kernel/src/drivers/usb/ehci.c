#include "ehci.h"
#include "../../apic/lapic_timer.h"
#include "../../console/klog.h"
#include "../../cpu/irq.h"
#include "../../io/io.h"
#include "../../lib/string.h"
#include "../../mm/dma_alloc.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../pci/pci.h"
#include "usb.h"
#include "usb_kbd.h"
#include "usb_mouse.h"
#include <stddef.h>

static struct ehci_controller controllers[EHCI_MAX_CONTROLLERS];
static int ehci_count = 0;

// Global interrupt pipe array (shared across all EHCI controllers)
static struct ehci_int_pipe int_pipes[EHCI_MAX_INT_PIPES];
static int int_pipe_count = 0;

// Helpers
static void ehci_enumerate_ports(struct ehci_controller *hc);

/* io_wait() writes to legacy port 0x80 and therefore causes a VM exit for
 * every iteration under KVM.  EHCI waits are measured in real milliseconds;
 * use the already calibrated monotonic clock and PAUSE while polling. */
static void ehci_delay_ms(uint32_t ms) {
  uint64_t deadline = lapic_timer_get_ms() + ms;
  while (lapic_timer_get_ms() < deadline)
    hal_cpu_relax();
}

static inline uint32_t ehci_read_cap32(struct ehci_controller *hc,
                                       uint32_t reg) {
  return *(volatile uint32_t *)(hc->cap_base + reg);
}

static inline uint8_t ehci_read_cap8(struct ehci_controller *hc, uint32_t reg) {
  return *(volatile uint8_t *)(hc->cap_base + reg);
}

static inline uint32_t ehci_read_op(struct ehci_controller *hc, uint32_t reg) {
  return *(volatile uint32_t *)(hc->op_base + reg);
}

static inline void ehci_write_op(struct ehci_controller *hc, uint32_t reg,
                                 uint32_t val) {
  *(volatile uint32_t *)(hc->op_base + reg) = val;
}

// BIOS Handover (EECP)

static void ehci_bios_handover(struct ehci_controller *hc) {
  uint32_t hccparams = ehci_read_cap32(hc, EHCI_CAP_HCCPARAMS);
  uint8_t eecp_offset = (hccparams >> 8) & 0xFF;

  if (eecp_offset < 0x40) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " EHCI: No EECP found (BIOS handover not needed).\n");
    return;
  }

  uint32_t legsup =
      pci_config_read32(hc->pci_bus, hc->pci_slot, hc->pci_func, eecp_offset);

  // Officially claim ownership
  pci_config_write32(hc->pci_bus, hc->pci_slot, hc->pci_func, eecp_offset,
                     legsup | EHCI_LEGACY_OS_OWNED);

  if (legsup & EHCI_LEGACY_BIOS_OWNED) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " EHCI: BIOS owns controller. Waiting for handover...\n");
    uint64_t deadline = lapic_timer_get_ms() + 1000;
    while (lapic_timer_get_ms() < deadline) {
      legsup = pci_config_read32(hc->pci_bus, hc->pci_slot, hc->pci_func,
                                 eecp_offset);
      if (!(legsup & EHCI_LEGACY_BIOS_OWNED))
        break;
      hal_cpu_relax();
    }
  }

  // Disable SMI on USB events
  pci_config_write32(hc->pci_bus, hc->pci_slot, hc->pci_func, eecp_offset + 4,
                     0);
}

// Reset and Initialization

static void ehci_controller_reset(struct ehci_controller *hc) {
  // 1. Stop the controller if it's running
  uint32_t cmd = ehci_read_op(hc, EHCI_REG_USBCMD);
  cmd &= ~EHCI_CMD_RS;
  ehci_write_op(hc, EHCI_REG_USBCMD, cmd);

  // Wait for it to stop
  uint64_t stop_deadline = lapic_timer_get_ms() + 100;
  while (!(ehci_read_op(hc, EHCI_REG_USBSTS) & EHCI_STS_HALTED) &&
         lapic_timer_get_ms() < stop_deadline)
    hal_cpu_relax();

  // 2. Issue Reset
  ehci_write_op(hc, EHCI_REG_USBCMD, EHCI_CMD_HCRESET);

  // 3. Wait for reset to complete
  uint64_t reset_deadline = lapic_timer_get_ms() + 1000;
  while (ehci_read_op(hc, EHCI_REG_USBCMD) & EHCI_CMD_HCRESET) {
    if (lapic_timer_get_ms() >= reset_deadline) {
      klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " EHCI: Reset timed out!\n");
      return;
    }
    hal_cpu_relax();
  }
}

static void ehci_init_schedule(struct ehci_controller *hc) {
  // Asynchronous Schedule
  // Allocate Dummy QH for Asynchronous Schedule
  uint64_t phys;
  hc->async_qh = (struct ehci_qh *)dma_alloc_page(&phys);
  memset(hc->async_qh, 0, 4096);
  hc->async_qh_phys = (uint32_t)phys;

  // Initialize Dummy QH
  hc->async_qh->link = hc->async_qh_phys | EHCI_PTR_QH;
  hc->async_qh->ep_char = (1 << 15); // H (Head of Reclamation) bit
  hc->async_qh->overlay.next = EHCI_PTR_TERMINATE;
  hc->async_qh->overlay.alt_next = EHCI_PTR_TERMINATE;
  hc->async_qh->overlay.token = 0; // Not active

  // Set the Asynchronous List Address
  ehci_write_op(hc, EHCI_REG_ASYNCLISTADDR, hc->async_qh_phys);

  // Periodic Schedule
  // Allocate the 4KB Periodic Frame List (1024 × 32-bit pointers)
  hc->periodic_list = (uint32_t *)dma_alloc_page(&phys);
  hc->periodic_list_phys = (uint32_t)phys;

  // Initialize all entries to TERMINATE (no interrupt QHs linked yet)
  for (int i = 0; i < EHCI_PERIODIC_FRAME_COUNT; i++) {
    hc->periodic_list[i] = EHCI_PTR_TERMINATE;
  }

  // Tell the hardware where the periodic list is
  ehci_write_op(hc, EHCI_REG_PERIODICLISTBASE, hc->periodic_list_phys);

  // Interrupt Pipe Pools (QHs + qTDs for HID devices)
  hc->int_qh_pool = (struct ehci_qh *)dma_alloc_page(&phys);
  memset(hc->int_qh_pool, 0, 4096);
  hc->int_qh_pool_phys = (uint32_t)phys;

  hc->int_qtd_pool = (struct ehci_qtd *)dma_alloc_page(&phys);
  memset(hc->int_qtd_pool, 0, 4096);
  hc->int_qtd_pool_phys = (uint32_t)phys;

  // Control Transfer Pools
  hc->qh_pool = (struct ehci_qh *)dma_alloc_page(&phys);
  memset(hc->qh_pool, 0, 4096);
  hc->qh_pool_phys = (uint32_t)phys;

  hc->qtd_pool = (struct ehci_qtd *)dma_alloc_page(&phys);
  memset(hc->qtd_pool, 0, 4096);
  hc->qtd_pool_phys = (uint32_t)phys;

  hc->transfer_buffer = dma_alloc_page(&phys);
  hc->transfer_buffer_phys = (uint32_t)phys;
}

// Port Management

static void ehci_reset_port(struct ehci_controller *hc, uint8_t port) {
  uint32_t reg = EHCI_REG_PORTSC + (port * 4);
  uint32_t status = ehci_read_op(hc, reg);

  // 1. Kick off reset
  // The spec says write 0 to bit 2 (Port Enable) and 1 to bit 8 (Port Reset).
  // Preserve connect status, clear enable and change bits, set reset.
  status &= ~(EHCI_PORT_ENABLE | EHCI_PORT_EN_CHANGE);
  status |= EHCI_PORT_RESET;
  ehci_write_op(hc, reg, status);

  // 2. Hold reset for 50ms (USB 2.0 spec requires at least 50ms for root ports)
  ehci_delay_ms(50);

  // 3. Clear reset
  status = ehci_read_op(hc, reg);
  status &= ~EHCI_PORT_RESET;
  ehci_write_op(hc, reg, status);

  // 4. Wait for reset bit to actually clear (HC may take a few uframes)
  uint64_t clear_deadline = lapic_timer_get_ms() + 10;
  while (lapic_timer_get_ms() < clear_deadline) {
    status = ehci_read_op(hc, reg);
    if (!(status & EHCI_PORT_RESET))
      break;
    hal_cpu_relax();
  }

  // 5. Post-reset recovery delay (USB 2.0 spec TRSTRCY = 10ms minimum)
  ehci_delay_ms(10);

  // 6. Re-read status after recovery
  status = ehci_read_op(hc, reg);

  // 7. Clear any status change bits that got set during reset
  uint32_t clear_bits = status & (EHCI_PORT_EN_CHANGE | (1 << 3) | (1 << 5));
  if (clear_bits) {
    ehci_write_op(hc, reg, status | clear_bits);
    status = ehci_read_op(hc, reg);
  }

  // 8. If port is ENABLED, it's High-Speed (or QEMU FS/LS via built-in TT).
  // If port is NOT enabled, it's FS/LS requiring companion controller.
  if (!(status & EHCI_PORT_ENABLE)) {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " EHCI: Port ");
    klog_uint64(port);
    klog_puts(": Not enabled after reset (FS/LS). Handing to companion...\n");
    status |= EHCI_PORT_OWNER;
    ehci_write_op(hc, reg, status);
  } else {
    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " EHCI: Port ");
    klog_uint64(port);
    klog_puts(": Device enabled (portsc=0x");
    klog_hex32(status);
    klog_puts(")\n");
  }
}

// Control Transfers

int ehci_control_transfer(struct ehci_controller *hc, uint8_t addr,
                          struct usb_control_request *req, void *data,
                          uint16_t len, bool low_speed) {
  (void)low_speed;

  // Setup Stage qTD
  struct ehci_qtd *setup_qtd = &hc->qtd_pool[0];
  struct ehci_qtd *data_qtd = (len > 0) ? &hc->qtd_pool[1] : NULL;
  struct ehci_qtd *status_qtd = (len > 0) ? &hc->qtd_pool[2] : &hc->qtd_pool[1];

  uint32_t setup_phys = hc->qtd_pool_phys + (0 * sizeof(struct ehci_qtd));
  uint32_t data_phys = hc->qtd_pool_phys + (1 * sizeof(struct ehci_qtd));
  uint32_t status_phys =
      hc->qtd_pool_phys + ((len > 0 ? 2 : 1) * sizeof(struct ehci_qtd));

  memset(hc->qtd_pool, 0, 3 * sizeof(struct ehci_qtd));
  memcpy(hc->transfer_buffer, req, sizeof(struct usb_control_request));

  setup_qtd->next = (len > 0) ? data_phys : status_phys;
  setup_qtd->alt_next = EHCI_PTR_TERMINATE;
  setup_qtd->token = (QTD_PID_SETUP << 8) | (3 << 10) |
                     (sizeof(struct usb_control_request) << 16) |
                     QTD_TOKEN_ACTIVE;
  setup_qtd->buffer[0] = hc->transfer_buffer_phys;

  if (data_qtd) {
    bool is_in = (req->request_type & 0x80) != 0;
    if (!is_in && data)
      memcpy((uint8_t *)hc->transfer_buffer + 64, data, len);
    data_qtd->next = status_phys;
    data_qtd->alt_next = EHCI_PTR_TERMINATE;
    data_qtd->token = ((is_in ? QTD_PID_IN : QTD_PID_OUT) << 8) | (3 << 10) |
                      (len << 16) | (1 << 31) | QTD_TOKEN_ACTIVE;
    data_qtd->buffer[0] = hc->transfer_buffer_phys + 64;
  }

  bool status_in = (len == 0 || !(req->request_type & 0x80));
  status_qtd->next = EHCI_PTR_TERMINATE;
  status_qtd->alt_next = EHCI_PTR_TERMINATE;
  status_qtd->token = ((status_in ? QTD_PID_IN : QTD_PID_OUT) << 8) |
                      (3 << 10) | (0 << 16) | (1 << 31) | QTD_TOKEN_ACTIVE;

  struct ehci_qh *qh = &hc->qh_pool[0];
  uint32_t qh_phys = hc->qh_pool_phys;
  memset(qh, 0, sizeof(struct ehci_qh));
  qh->link = EHCI_PTR_TERMINATE;
  qh->ep_char = addr | (0 << 8) | (64 << 16) | (2 << 12) | (1 << 14);
  qh->ep_caps = (1 << 30);
  qh->overlay.next = setup_phys;

  asm volatile("mfence" ::: "memory");
  qh->link = hc->async_qh->link;
  hc->async_qh->link = qh_phys | EHCI_PTR_QH;
  asm volatile("mfence" ::: "memory");

  bool success = false;
  uint64_t transfer_deadline = lapic_timer_get_ms() + 500;
  while (lapic_timer_get_ms() < transfer_deadline) {
    asm volatile("" ::: "memory");
    if (!(status_qtd->token & QTD_TOKEN_ACTIVE)) {
      if (!(status_qtd->token & 0x7C))
        success = true;
      break;
    }
    hal_cpu_relax();
  }

  hc->async_qh->link = qh->link;
  asm volatile("mfence" ::: "memory");

  if (success && data && len > 0 && (req->request_type & 0x80)) {
    memcpy(data, (uint8_t *)hc->transfer_buffer + 64, len);
  }

  return success ? 0 : -1;
}

static int ehci_hcd_control_transfer(struct usb_hcd *hcd, uint8_t addr,
                                     struct usb_control_request *req,
                                     void *data, uint16_t len,
                                     enum usb_speed speed) {
  struct ehci_controller *hc = (struct ehci_controller *)hcd->priv;
  return ehci_control_transfer(hc, addr, req, data, len,
                               usb_speed_is_low(speed));
}


//
// EHCI Periodic Schedule uses QHs linked into the Periodic Frame List.
// Each QH points to a qTD that performs an IN transfer from the device's
// interrupt endpoint. We use a ping-pong scheme (2 qTDs) for clean resubmit.

struct ehci_int_pipe *ehci_setup_int_in(struct ehci_controller *hc,
                                        uint8_t dev_addr, uint8_t ep_num,
                                        uint16_t max_packet, uint8_t interval,
                                        bool low_speed, void *buffer,
                                        uint32_t buffer_phys) {
  (void)low_speed; // EHCI only handles high-speed devices

  if (int_pipe_count >= EHCI_MAX_INT_PIPES)
    return NULL;

  struct ehci_int_pipe *pipe = &int_pipes[int_pipe_count];
  pipe->hc = hc;
  pipe->data_buf = buffer;
  pipe->data_buf_phys = buffer_phys;
  pipe->max_packet = max_packet;
  pipe->interval = interval;
  pipe->active = true;
  pipe->cur_idx = 0;

  // Allocate QH and 2 qTDs from the dedicated interrupt pools
  int pipe_idx = int_pipe_count;

  pipe->qh = &hc->int_qh_pool[pipe_idx];
  pipe->qh_phys = hc->int_qh_pool_phys + (pipe_idx * sizeof(struct ehci_qh));

  pipe->qtd[0] = &hc->int_qtd_pool[pipe_idx * 2];
  pipe->qtd_phys[0] =
      hc->int_qtd_pool_phys + (pipe_idx * 2 * sizeof(struct ehci_qtd));

  pipe->qtd[1] = &hc->int_qtd_pool[pipe_idx * 2 + 1];
  pipe->qtd_phys[1] =
      hc->int_qtd_pool_phys + ((pipe_idx * 2 + 1) * sizeof(struct ehci_qtd));

  // Build the QH
  memset(pipe->qh, 0, sizeof(struct ehci_qh));

  // ep_char: device address, endpoint number, high speed, max packet size
  // Bit 14 = DTC (Data Toggle Control from qTD)
  pipe->qh->ep_char = ((uint32_t)dev_addr) | ((uint32_t)ep_num << 8) |
                      QH_EP_SPEED_HIGH | ((uint32_t)max_packet << 16) |
                      (1 << 14); // DTC = 1 (toggle from qTD)

  // ep_caps: Mult = 1 (one transaction per microframe minimum)
  // High-speed interrupt endpoints: S-mask determines which microframes to
  // schedule. We use microframe 0 (bit 0 of S-mask) for simplicity.
  pipe->qh->ep_caps = (1 << 30) | // Mult = 1
                      (0x01);     // S-mask = microframe 0

  // Build qTD[0] as the active transfer
  memset(pipe->qtd[0], 0, sizeof(struct ehci_qtd));
  pipe->qtd[0]->next = EHCI_PTR_TERMINATE;
  pipe->qtd[0]->alt_next = EHCI_PTR_TERMINATE;
  pipe->qtd[0]->token = (QTD_PID_IN << 8) | (3 << 10) | // C_ERR = 3
                        (1 << 15) | // IOC = generate interrupt
                        ((uint32_t)max_packet << 16) | // Total bytes
                        QTD_TOKEN_ACTIVE; // Data toggle = 0 (first xfer)
  pipe->qtd[0]->buffer[0] = buffer_phys;

  // qTD[1] is the inactive spare (for ping-pong resubmit)
  memset(pipe->qtd[1], 0, sizeof(struct ehci_qtd));

  // Link QH overlay to our active qTD
  pipe->qh->overlay.next = pipe->qtd_phys[0];
  pipe->qh->overlay.alt_next = EHCI_PTR_TERMINATE;
  pipe->qh->overlay.token = 0; // HC will override this from qtd[0]

  // Insert QH into the Periodic Frame List
  // The interval determines how many milliseconds between polls.
  // EHCI operates at 1 frame/ms; we insert the QH every N frames.
  uint16_t sched_interval = 1;
  if (interval > 1) {
    sched_interval = interval;
    // Round down to power of 2
    uint16_t p2 = 1;
    while (p2 * 2 <= sched_interval && p2 < 512)
      p2 *= 2;
    sched_interval = p2;
  }
  if (sched_interval > 1024)
    sched_interval = 1024;

  asm volatile("mfence" ::: "memory");
  for (int i = 0; i < EHCI_PERIODIC_FRAME_COUNT; i += sched_interval) {
    // Chain: framelist[i] → our QH → previous entry
    pipe->qh->link = hc->periodic_list[i]; // Chain to existing
    hc->periodic_list[i] = pipe->qh_phys | EHCI_PTR_QH;
  }
  asm volatile("mfence" ::: "memory");

  // Enable Periodic Schedule if not already enabled
  uint32_t cmd = ehci_read_op(hc, EHCI_REG_USBCMD);
  if (!(cmd & EHCI_CMD_PSE)) {
    cmd |= EHCI_CMD_PSE;
    ehci_write_op(hc, EHCI_REG_USBCMD, cmd);

    // Wait for PSS (Periodic Schedule Status) to confirm activation
    for (int i = 0; i < 100000; i++) {
      if (ehci_read_op(hc, EHCI_REG_USBSTS) & EHCI_STS_PSS)
        break;
      io_wait();
    }
  }

  int_pipe_count++;

  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " EHCI: Interrupt IN pipe: addr=");
  klog_uint64(dev_addr);
  klog_puts(", ep=");
  klog_uint64(ep_num);
  klog_puts(", mps=");
  klog_uint64(max_packet);
  klog_puts(", interval=");
  klog_uint64(sched_interval);
  klog_puts("ms\n");

  return pipe;
}

bool ehci_int_pipe_completed(struct ehci_int_pipe *pipe) {
  if (!pipe || !pipe->active)
    return false;
  asm volatile("" ::: "memory");
  // Check the overlay area of the QH — the HC copies qTD status here
  // When the Active bit clears, the transfer is done
  uint32_t token = pipe->qh->overlay.token;
  return !(token & QTD_TOKEN_ACTIVE);
}

void ehci_int_pipe_resubmit(struct ehci_int_pipe *pipe) {
  if (!pipe || !pipe->active)
    return;

  int old_active = pipe->cur_idx;
  int new_active = 1 - old_active;

  // Get the toggle bit from the completed transfer (preserved in overlay)
  uint32_t old_token = pipe->qh->overlay.token;
  uint32_t toggle = old_token & (1 << 31); // Data toggle bit
  toggle ^= (1 << 31);                     // Flip for next transfer

  // Build the new active qTD
  struct ehci_qtd *qtd = pipe->qtd[new_active];
  memset(qtd, 0, sizeof(struct ehci_qtd));
  qtd->next = EHCI_PTR_TERMINATE;
  qtd->alt_next = EHCI_PTR_TERMINATE;
  qtd->token = (QTD_PID_IN << 8) | (3 << 10) |      // C_ERR = 3
               (1 << 15) |                          // IOC = generate interrupt
               ((uint32_t)pipe->max_packet << 16) | // Total bytes
               toggle |                             // Data toggle
               QTD_TOKEN_ACTIVE;
  qtd->buffer[0] = pipe->data_buf_phys;

  pipe->cur_idx = new_active;

  asm volatile("mfence" ::: "memory");

  // Repoint the QH overlay to the new qTD
  // The HC will pick this up on the next periodic schedule traversal
  pipe->qh->overlay.next = pipe->qtd_phys[new_active];
  pipe->qh->overlay.alt_next = EHCI_PTR_TERMINATE;
  pipe->qh->overlay.token = 0; // Clear — HC reloads from qTD
  pipe->qh->current = 0;       // Force re-fetch

  asm volatile("mfence" ::: "memory");
}

// IRQ Handler

static void ehci_irq_handler(struct registers *regs) {
  (void)regs;
  for (int i = 0; i < ehci_count; i++) {
    struct ehci_controller *hc = &controllers[i];
    if (!hc->present)
      continue;

    uint32_t status = ehci_read_op(hc, EHCI_REG_USBSTS);
    uint32_t enabled = ehci_read_op(hc, EHCI_REG_USBINTR);
    uint32_t active = status & enabled;

    if (active == 0)
      continue;

    hc->interrupts++;
    // Acknowledge by writing back the active bits
    ehci_write_op(hc, EHCI_REG_USBSTS, active);

    if (active & EHCI_STS_USBINT) {
      // USB Interrupt — a transfer completed (IOC)
      // Poll HID devices to check their pipe status
      usb_kbd_poll();
      usb_mouse_poll();
    }

    if (active & EHCI_STS_ERROR) {
      klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " EHCI USB Error Interrupt\n");
    }

    if (active & EHCI_STS_HSE) {
      klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " EHCI: Host System Error!\n");
    }

    if (active & EHCI_STS_PCD) {
      // Port Change Detect — could handle hot-plug here
    }
  }
}

// Initialization Entry

void ehci_init(void) {
  ehci_count = 0;
  int_pipe_count = 0;
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Searching for EHCI controllers...\n");

  for (uint32_t i = 0; i < pci_get_device_count(); i++) {
    struct pci_device *pdev = pci_get_device(i);
    if (pdev->class_code == 0x0C && pdev->subclass == 0x03 &&
        pdev->prog_if == 0x20) {
      if (ehci_count >= EHCI_MAX_CONTROLLERS)
        break;

      struct ehci_controller *hc = &controllers[ehci_count++];
      hc->pci_bus = pdev->bus;
      hc->pci_slot = pdev->slot;
      hc->pci_func = pdev->func;
      hc->vendor_id = pdev->vendor_id;
      hc->device_id = pdev->device_id;
      hc->irq_line = pdev->irq_line;

      uint32_t pci_cmd =
          pci_config_read32(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04);
      pci_config_write32(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04,
                         pci_cmd | 0x06);

      uintptr_t phys_base = pdev->bar[0] & 0xFFFFFFF0;
      uintptr_t virt_base = phys_base + pmm_get_hhdm_offset();

      /* The kernel HHDM already covers physical MMIO addresses. Reinstalling
       * these mappings is both redundant and unsafe during SMP early boot: it
       * turns a simple probe into a synchronous global TLB shootdown before
       * the APs are doing useful work. */

      hc->cap_base = virt_base;
      uint8_t cap_len = ehci_read_cap8(hc, EHCI_CAP_CAPLENGTH);
      hc->op_base = hc->cap_base + cap_len;

      uint32_t hcsparams = ehci_read_cap32(hc, EHCI_CAP_HCSPARAMS);
      hc->num_ports = hcsparams & 0x0F;

      ehci_bios_handover(hc);

      klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " EHCI: Resetting controller...\n");
      ehci_controller_reset(hc);

      klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " EHCI: Initializing schedules...\n");
      ehci_init_schedule(hc);

      // Register IRQ handler for EHCI interrupts
      hc->irq_registered =
          irq_install_handler(hc->irq_line, ehci_irq_handler, 0x000F);
      if (hc->irq_registered) {
        // Enable USB Interrupt, Error, Port Change, and Host System Error
        ehci_write_op(hc, EHCI_REG_USBINTR,
                      EHCI_INTR_USBINT | EHCI_INTR_ERROR | EHCI_INTR_PCD |
                          EHCI_INTR_HSE);
      }

      // Clear segment register (we use 32-bit addresses)
      ehci_write_op(hc, EHCI_REG_CTRLDSSEGMENT, 0);

      uint32_t cmd = ehci_read_op(hc, EHCI_REG_USBCMD);
      cmd |= EHCI_CMD_RS | EHCI_CMD_ASE;
      ehci_write_op(hc, EHCI_REG_USBCMD, cmd);

      // Wait for controller to start running
      uint64_t run_deadline = lapic_timer_get_ms() + 100;
      while (lapic_timer_get_ms() < run_deadline) {
        if (!(ehci_read_op(hc, EHCI_REG_USBSTS) & EHCI_STS_HALTED))
          break;
        hal_cpu_relax();
      }

      // Route all ports to EHCI first, then hand low-speed to companion UHCI
      // This MUST happen before UHCI probes ports
      ehci_write_op(hc, EHCI_REG_CONFIGFLAG, 1);

      // Wait 200ms for ports to detect connections after CONFIGFLAG is set.
      // The USB 2.0 spec requires time for port routing and device detection.
      // Without this delay, port enumeration may find no connected devices.
      ehci_delay_ms(200);

      hc->hcd.priv = hc;
      hc->hcd.name = "ehci";
      hc->hcd.control_transfer = ehci_hcd_control_transfer;
      hc->hcd.address_device = NULL;
      hc->hcd.device_removed = NULL;

      hc->present = true;
    }
  }

  for (int i = 0; i < ehci_count; i++) {
    ehci_enumerate_ports(&controllers[i]);
  }
}

void ehci_hand_to_companion(void) {
  // Hand low-speed/full-speed ports to companion UHCI/OHCI controllers.
  // Ports with ENABLED devices must stay under EHCI ownership.
  // Ports with NO device connected should also stay under EHCI ownership
  // (so hot-plug detection still works through EHCI).
  // Only hand over ports that have a CONNECTED device but failed to ENABLE
  // (indicating a FS/LS device that EHCI can't handle natively).
  for (int i = 0; i < ehci_count; i++) {
    struct ehci_controller *hc = &controllers[i];
    for (uint8_t p = 0; p < hc->num_ports; p++) {
      uint32_t portsc = ehci_read_op(hc, EHCI_REG_PORTSC + (p * 4));

      // Skip ports that are already enabled (device is working under EHCI)
      if (portsc & EHCI_PORT_ENABLE)
        continue;

      // Skip ports with no device connected — keep them under EHCI
      if (!(portsc & EHCI_PORT_CONNECT))
        continue;

      // Skip ports already owned by companion
      if (portsc & EHCI_PORT_OWNER)
        continue;

      // This port has a connected device that didn't enable under EHCI
      // (FS/LS device). Hand it to the companion controller.
       klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " EHCI: Handing port ");
      klog_uint64(p);
      klog_puts(" to companion controller\n");
      portsc |= EHCI_PORT_OWNER;
      ehci_write_op(hc, EHCI_REG_PORTSC + (p * 4), portsc);
    }
  }
}


// After resetting ports, enumerate high-speed devices through the USB core
// which will trigger HID driver probe (keyboard/mouse).

static void ehci_enumerate_ports(struct ehci_controller *hc) {
  klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " EHCI: Scanning ");
  klog_uint64(hc->num_ports);
  klog_puts(" ports...\n");

  for (uint8_t p = 0; p < hc->num_ports; p++) {
    uint32_t portsc = ehci_read_op(hc, EHCI_REG_PORTSC + (p * 4));

    /* Empty root ports are the normal case.  Avoid six misleading failure
     * lines (and slow serial output) on every boot. */
    if (!(portsc & EHCI_PORT_CONNECT))
      continue;

    klog_puts("       Port ");
    klog_uint64(p);
    klog_puts(" connected (PORTSC=");
    klog_hex32(portsc);
    klog_puts("); resetting...\n");
    ehci_reset_port(hc, p);

    // After reset, if it's still EHCI-enabled, enumerate it
    portsc = ehci_read_op(hc, EHCI_REG_PORTSC + (p * 4));
    if (portsc & EHCI_PORT_ENABLE) {
      // Device is enabled — enumerate through USB core
      // This will call usb_device_discovered → usb_enumerate_device →
      // usb_kbd_probe / usb_mouse_probe
      usb_device_discovered(&hc->hcd, p, USB_SPEED_HIGH);
    } else {
      klog_puts("       Port ");
      klog_uint64(p);
      klog_puts(": Not enabled after reset (PORTSC=0x");
      klog_hex32(portsc);
      klog_puts(")\n");
    }
  }
}

// Public API

int ehci_get_controller_count(void) { return ehci_count; }

struct ehci_controller *ehci_get_controller(int index) {
  if (index < 0 || index >= ehci_count)
    return NULL;
  return &controllers[index];
}
