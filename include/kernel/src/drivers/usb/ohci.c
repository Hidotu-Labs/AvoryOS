#ifndef DRIVERS_USB_OHCI_C
#define DRIVERS_USB_OHCI_C

#include "ohci.h"
#include "../../console/klog.h"
#include "../../cpu/irq.h"
#include "../../io/io.h"
#include "../../mm/dma_alloc.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../pci/pci.h"
#include "usb.h"
#include "usb_kbd.h"
#include "usb_mouse.h"
#include <stddef.h>

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

static struct ohci_controller controllers[OHCI_MAX_CONTROLLERS];
static int controller_count = 0;

// Global interrupt pipe array
static struct ohci_int_pipe int_pipes[OHCI_MAX_INT_PIPES];
static int int_pipe_count = 0;

// Register Access Helpers

static inline uint32_t ohci_read32(struct ohci_controller *hc, uint16_t reg) {
  return *(volatile uint32_t *)(hc->mmio_base + reg);
}

static inline void ohci_write32(struct ohci_controller *hc, uint16_t reg,
                                uint32_t val) {
  *(volatile uint32_t *)(hc->mmio_base + reg) = val;
}

// Pool Allocators
// Interrupt pipes use EDs 0..7 and TDs 0..15 (persistent, never reset).
// Control transfers use EDs 8+ and TDs 16+ (reset between each transfer).

static struct ohci_ed *ohci_alloc_int_ed(struct ohci_controller *hc,
                                         uint32_t *phys_out) {
  if (hc->int_ed_used >= OHCI_INT_ED_MAX)
    return NULL;
  int idx = hc->int_ed_used++;
  struct ohci_ed *ed = &hc->ed_pool[idx];
  ed->control = 0;
  ed->tail_p = 0;
  ed->head_p = 0;
  ed->next_ed = 0;
  if (phys_out)
    *phys_out = hc->ed_pool_phys + (idx * sizeof(struct ohci_ed));
  return ed;
}

static struct ohci_td *ohci_alloc_int_td(struct ohci_controller *hc,
                                         uint32_t *phys_out) {
  if (hc->int_td_used >= OHCI_INT_TD_MAX)
    return NULL;
  int idx = hc->int_td_used++;
  struct ohci_td *td = &hc->td_pool[idx];
  td->control = 0;
  td->buffer_p = 0;
  td->next_td = 0;
  td->buffer_end = 0;
  if (phys_out)
    *phys_out = hc->td_pool_phys + (idx * sizeof(struct ohci_td));
  return td;
}

static struct ohci_ed *ohci_alloc_ed(struct ohci_controller *hc,
                                     uint32_t *phys_out) {
  if (hc->ed_used >= OHCI_MAX_EDS)
    return NULL;
  int idx = hc->ed_used++;
  struct ohci_ed *ed = &hc->ed_pool[idx];
  ed->control = 0;
  ed->tail_p = 0;
  ed->head_p = 0;
  ed->next_ed = 0;
  if (phys_out)
    *phys_out = hc->ed_pool_phys + (idx * sizeof(struct ohci_ed));
  return ed;
}

static struct ohci_td *ohci_alloc_td(struct ohci_controller *hc,
                                     uint32_t *phys_out) {
  if (hc->td_used >= OHCI_MAX_TDS)
    return NULL;
  int idx = hc->td_used++;
  struct ohci_td *td = &hc->td_pool[idx];
  td->control = 0;
  td->buffer_p = 0;
  td->next_td = 0;
  td->buffer_end = 0;
  if (phys_out)
    *phys_out = hc->td_pool_phys + (idx * sizeof(struct ohci_td));
  return td;
}

static void ohci_reset_pools(struct ohci_controller *hc) {
  // Only reset the control transfer region, preserving interrupt pipe state
  hc->ed_used = OHCI_CTRL_ED_START;
  hc->td_used = OHCI_CTRL_TD_START;
  uint8_t *p = (uint8_t *)&hc->ed_pool[OHCI_CTRL_ED_START];
  for (int i = 0;
       i < (int)((OHCI_MAX_EDS - OHCI_CTRL_ED_START) * sizeof(struct ohci_ed));
       i++)
    p[i] = 0;
  p = (uint8_t *)&hc->td_pool[OHCI_CTRL_TD_START];
  for (int i = 0;
       i < (int)((OHCI_MAX_TDS - OHCI_CTRL_TD_START) * sizeof(struct ohci_td));
       i++)
    p[i] = 0;
}



struct ohci_int_pipe *ohci_setup_int_in(struct ohci_controller *hc,
                                        uint8_t dev_addr, uint8_t ep_num,
                                        uint16_t max_packet, uint8_t interval,
                                        bool low_speed, void *buffer,
                                        uint32_t buffer_phys) {
  if (int_pipe_count >= OHCI_MAX_INT_PIPES)
    return NULL;

  struct ohci_int_pipe *pipe = &int_pipes[int_pipe_count];
  pipe->hc = hc;
  pipe->data_buf = buffer;
  pipe->data_buf_phys = buffer_phys;
  pipe->max_packet = max_packet;
  pipe->active = true;
  pipe->cur_idx = 0;

  // Allocate ED and 2 TDs from the persistent interrupt region
  pipe->ed = ohci_alloc_int_ed(hc, &pipe->ed_phys);
  pipe->td[0] = ohci_alloc_int_td(hc, &pipe->td_phys[0]);
  pipe->td[1] = ohci_alloc_int_td(hc, &pipe->td_phys[1]);

  if (!pipe->ed || !pipe->td[0] || !pipe->td[1]) {
    klog_puts("[OHCI] Failed to allocate interrupt pipe resources\n");
    pipe->active = false;
    return NULL;
  }

  // Build the ED: direction=IN, speed, MPS, endpoint, address
  pipe->ed->control = ((uint32_t)dev_addr << OHCI_ED_FA_SHIFT) |
                      ((uint32_t)ep_num << OHCI_ED_EN_SHIFT) | OHCI_ED_DIR_IN |
                      OHCI_ED_FMT_GEN |
                      ((uint32_t)max_packet << OHCI_ED_MPS_SHIFT) |
                      (low_speed ? OHCI_ED_SPEED_LOW : OHCI_ED_SPEED_FULL);

  // Build TD[0] as the active transfer TD
  pipe->td[0]->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_DP_IN |
                         OHCI_TD_DI_IMM | OHCI_TD_ROUNDING | OHCI_TD_TOGGLE_ED;
  pipe->td[0]->buffer_p = buffer_phys;
  pipe->td[0]->buffer_end = buffer_phys + max_packet - 1;
  pipe->td[0]->next_td = pipe->td_phys[1]; // Chain to dummy

  // TD[1] is the dummy (tail) — HC stops when head == tail
  pipe->td[1]->control = 0;
  pipe->td[1]->buffer_p = 0;
  pipe->td[1]->buffer_end = 0;
  pipe->td[1]->next_td = 0;

  // Link into ED: head = active TD, tail = dummy TD
  pipe->ed->head_p = pipe->td_phys[0]; // toggleCarry starts at 0 (DATA0)
  pipe->ed->tail_p = pipe->td_phys[1];

  // Insert ED into all 32 HCCA interrupt table slots for maximum polling rate.
  // All slots start with the same chain, so next_ed is consistent.
  asm volatile("mfence" ::: "memory");
  for (int i = 0; i < 32; i++) {
    pipe->ed->next_ed = hc->hcca->interrupt_table[i];
    hc->hcca->interrupt_table[i] = pipe->ed_phys;
  }
  asm volatile("mfence" ::: "memory");

  // Enable Periodic List processing
  uint32_t ctrl = ohci_read32(hc, OHCI_REG_CONTROL);
  ctrl |= OHCI_CTRL_PLE;
  ohci_write32(hc, OHCI_REG_CONTROL, ctrl);

  int_pipe_count++;

  klog_puts("[OHCI] Interrupt IN pipe: addr=");
  klog_uint64(dev_addr);
  klog_puts(", ep=");
  klog_uint64(ep_num);
  klog_puts(", mps=");
  klog_uint64(max_packet);
  klog_puts("\n");

  return pipe;
}

bool ohci_int_pipe_completed(struct ohci_int_pipe *pipe) {
  if (!pipe || !pipe->active)
    return false;
  asm volatile("" ::: "memory");
  uint32_t cc =
      (pipe->td[pipe->cur_idx]->control & OHCI_TD_CC_MASK) >> OHCI_TD_CC_SHIFT;
  return cc != 0xF; // 0xF = not accessed yet
}

void ohci_int_pipe_resubmit(struct ohci_int_pipe *pipe) {
  if (!pipe || !pipe->active)
    return;

  int old_active = pipe->cur_idx;
  int new_active = 1 - old_active;

  // The old active TD completed. Swap roles:
  //   old active → new dummy
  //   old dummy  → new active (fill with transfer params)

  // Set up new active TD
  struct ohci_td *td = pipe->td[new_active];
  td->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_DP_IN | OHCI_TD_DI_IMM |
                OHCI_TD_ROUNDING | OHCI_TD_TOGGLE_ED;
  td->buffer_p = pipe->data_buf_phys;
  td->buffer_end = pipe->data_buf_phys + pipe->max_packet - 1;
  td->next_td = pipe->td_phys[old_active]; // Points to new dummy

  // Clear old active → new dummy
  struct ohci_td *dummy = pipe->td[old_active];
  dummy->control = 0;
  dummy->buffer_p = 0;
  dummy->buffer_end = 0;
  dummy->next_td = 0;

  pipe->cur_idx = new_active;

  asm volatile("mfence" ::: "memory");

  // Update ED pointers, preserving the toggleCarry bit from head_p
  uint32_t carry = pipe->ed->head_p & OHCI_ED_HEAD_CARRY;
  pipe->ed->tail_p = pipe->td_phys[old_active];         // New dummy tail
  pipe->ed->head_p = pipe->td_phys[new_active] | carry; // New active head

  asm volatile("mfence" ::: "memory");
}



static void ohci_reset_port(struct ohci_controller *hc, uint8_t port) {
  uint16_t reg = OHCI_REG_RH_PORT_STATUS + (port * 4);

  uint32_t status = ohci_read32(hc, reg);
  if (!(status & OHCI_PORT_PPS)) {
    ohci_write32(hc, reg, OHCI_PORT_PPS);
    for (int i = 0; i < 50000; i++)
      io_wait();
  }

  ohci_write32(hc, reg, OHCI_PORT_PRS);

  for (int i = 0; i < 100000; i++) {
    status = ohci_read32(hc, reg);
    if (!(status & OHCI_PORT_PRS))
      break;
    io_wait();
  }

  ohci_write32(hc, reg, OHCI_PORT_PRSC);
  for (int i = 0; i < 20000; i++)
    io_wait();

  status = ohci_read32(hc, reg);
  if (!(status & OHCI_PORT_PES)) {
    ohci_write32(hc, reg, OHCI_PORT_PES);
    for (int i = 0; i < 10000; i++)
      io_wait();
  }
}

static void ohci_probe_ports(struct ohci_controller *hc) {
  for (uint8_t i = 0; i < hc->num_ports; i++) {
    uint16_t reg = OHCI_REG_RH_PORT_STATUS + (i * 4);
    uint32_t status = ohci_read32(hc, reg);

    uint32_t changes =
        status & (OHCI_PORT_CSC | OHCI_PORT_PESC | OHCI_PORT_PSSC |
                  OHCI_PORT_OCIC | OHCI_PORT_PRSC);
    if (changes)
      ohci_write32(hc, reg, changes);

    if (status & OHCI_PORT_CCS) {
      klog_puts("[OHCI] Device detected on port ");
      klog_uint64(i + 1);
      bool low_speed = (status & OHCI_PORT_LSDA) != 0;
      klog_puts(low_speed ? " (Low-Speed)\n" : " (Full-Speed)\n");

      ohci_reset_port(hc, i);
      status = ohci_read32(hc, reg);
      if ((status & OHCI_PORT_CCS) && (status & OHCI_PORT_PES)) {
        low_speed = (status & OHCI_PORT_LSDA) != 0;
        usb_device_discovered(&hc->hcd, i,
                              low_speed ? USB_SPEED_LOW : USB_SPEED_FULL);
      } else {
        klog_puts("[OHCI] Port ");
        klog_uint64(i + 1);
        klog_puts(" reset failed (status=0x");
        klog_hex32(status);
        klog_puts(")\n");
      }
    }
  }
}



static int ohci_control_transfer(struct ohci_controller *hc, uint8_t addr,
                                 struct usb_control_request *req, void *data,
                                 uint16_t len, bool low_speed) {
  ohci_reset_pools(hc);

  struct usb_control_request *dma_req =
      (struct usb_control_request *)hc->transfer_buffer;
  *dma_req = *req;
  uint32_t setup_buf_phys = hc->transfer_buffer_phys;

  uint8_t *dma_data = (uint8_t *)hc->transfer_buffer + 64;
  uint32_t data_buf_phys = hc->transfer_buffer_phys + 64;

  if (data && len > 0 && !(req->request_type & 0x80)) {
    for (uint16_t i = 0; i < len; i++)
      dma_data[i] = ((uint8_t *)data)[i];
  }

  uint32_t setup_td_phys, status_td_phys, dummy_td_phys;
  uint32_t data_td_phys = 0;

  struct ohci_td *setup_td = ohci_alloc_td(hc, &setup_td_phys);
  struct ohci_td *data_td = NULL;
  if (len > 0)
    data_td = ohci_alloc_td(hc, &data_td_phys);
  struct ohci_td *status_td = ohci_alloc_td(hc, &status_td_phys);
  struct ohci_td *dummy_td = ohci_alloc_td(hc, &dummy_td_phys);

  if (!setup_td || !status_td || !dummy_td || (len > 0 && !data_td))
    return -1;

  setup_td->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_DP_SETUP |
                      OHCI_TD_DI_NONE | OHCI_TD_TOGGLE_0;
  setup_td->buffer_p = setup_buf_phys;
  setup_td->buffer_end =
      setup_buf_phys + sizeof(struct usb_control_request) - 1;

  if (data_td) {
    setup_td->next_td = data_td_phys;
    bool is_in = (req->request_type & 0x80) != 0;
    data_td->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_DI_NONE |
                       OHCI_TD_ROUNDING | OHCI_TD_TOGGLE_1 |
                       (is_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT);
    data_td->buffer_p = data_buf_phys;
    data_td->buffer_end = data_buf_phys + len - 1;
    data_td->next_td = status_td_phys;
  } else {
    setup_td->next_td = status_td_phys;
  }

  bool status_is_in = (len == 0 || !(req->request_type & 0x80));
  status_td->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_DI_IMM |
                       OHCI_TD_TOGGLE_1 |
                       (status_is_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT);
  status_td->buffer_p = 0;
  status_td->buffer_end = 0;
  status_td->next_td = dummy_td_phys;

  dummy_td->control = 0;
  dummy_td->buffer_p = 0;
  dummy_td->buffer_end = 0;
  dummy_td->next_td = 0;

  uint32_t ed_phys;
  struct ohci_ed *ed = ohci_alloc_ed(hc, &ed_phys);
  if (!ed)
    return -1;

  uint16_t mps = 8;
  ed->control = ((uint32_t)addr << OHCI_ED_FA_SHIFT) | (0 << OHCI_ED_EN_SHIFT) |
                OHCI_ED_DIR_TD | OHCI_ED_FMT_GEN |
                ((uint32_t)mps << OHCI_ED_MPS_SHIFT) |
                (low_speed ? OHCI_ED_SPEED_LOW : OHCI_ED_SPEED_FULL);
  ed->head_p = setup_td_phys;
  ed->tail_p = dummy_td_phys;
  ed->next_ed = 0;

  asm volatile("mfence" ::: "memory");
  ohci_write32(hc, OHCI_REG_CONTROL_HEAD_ED, ed_phys);
  uint32_t ctrl = ohci_read32(hc, OHCI_REG_CONTROL);
  ctrl |= OHCI_CTRL_CLE;
  ohci_write32(hc, OHCI_REG_CONTROL, ctrl);
  ohci_write32(hc, OHCI_REG_COMMAND_STATUS, OHCI_CMD_CLF);
  asm volatile("mfence" ::: "memory");

  bool timed_out = true;
  for (int i = 0; i < 2000000; i++) {
    asm volatile("" ::: "memory");
    uint32_t status_cc = status_td->control & OHCI_TD_CC_MASK;
    if (status_cc != OHCI_TD_CC_NOT_ACCESSED) {
      timed_out = false;
      break;
    }
    if (ed->head_p & OHCI_ED_HEAD_HALT) {
      timed_out = false;
      break;
    }
    for (int j = 0; j < 10; j++)
      io_wait();
  }

  ctrl = ohci_read32(hc, OHCI_REG_CONTROL);
  ctrl &= ~OHCI_CTRL_CLE;
  ohci_write32(hc, OHCI_REG_CONTROL, ctrl);
  ohci_write32(hc, OHCI_REG_CONTROL_HEAD_ED, 0);
  asm volatile("mfence" ::: "memory");

  uint32_t setup_cc = (setup_td->control & OHCI_TD_CC_MASK) >> OHCI_TD_CC_SHIFT;
  uint32_t data_cc =
      data_td ? (data_td->control & OHCI_TD_CC_MASK) >> OHCI_TD_CC_SHIFT : 0;
  uint32_t status_cc_final =
      (status_td->control & OHCI_TD_CC_MASK) >> OHCI_TD_CC_SHIFT;

  if (timed_out || setup_cc != 0 || (data_td && data_cc != 0) ||
      status_cc_final != 0) {
    klog_puts("[OHCI] Xfer fail:");
    if (timed_out)
      klog_puts(" TIMEOUT");
    klog_puts(" SETUP_CC=");
    klog_uint64(setup_cc);
    if (data_td) {
      klog_puts(" DATA_CC=");
      klog_uint64(data_cc);
    }
    klog_puts(" STATUS_CC=");
    klog_uint64(status_cc_final);
    klog_puts("\n");
    return -2;
  }

  if (data && len > 0 && (req->request_type & 0x80)) {
    for (uint16_t i = 0; i < len; i++)
      ((uint8_t *)data)[i] = dma_data[i];
  }

  return 0;
}

static int ohci_hcd_control_transfer(struct usb_hcd *hcd, uint8_t addr,
                                     struct usb_control_request *req,
                                     void *data, uint16_t len,
                                     enum usb_speed speed) {
  struct ohci_controller *hc = (struct ohci_controller *)hcd->priv;
  return ohci_control_transfer(hc, addr, req, data, len,
                               usb_speed_is_low(speed));
}

// Interrupt Handler

static void ohci_irq_handler(struct registers *regs) {
  (void)regs;
  for (int i = 0; i < controller_count; i++) {
    struct ohci_controller *hc = &controllers[i];
    if (!hc->present)
      continue;

    uint32_t status = ohci_read32(hc, OHCI_REG_INTERRUPT_STATUS);
    if (status == 0 || status == 0xFFFFFFFF)
      continue;

    hc->interrupts++;
    ohci_write32(hc, OHCI_REG_INTERRUPT_STATUS, status);

    if (status & OHCI_INTR_WDH) {
      // Clear done_head in HCCA (HC has written it)
      hc->hcca->done_head = 0;
      // Poll HID devices — they check their own pipe's TD status
      usb_kbd_poll();
      usb_mouse_poll();
    }

    if (status & OHCI_INTR_RHSC) {
      klog_puts("[OHCI] Root Hub Status Change detected\n");
      ohci_write32(hc, OHCI_REG_INTERRUPT_STATUS, OHCI_INTR_RHSC);
    }

    if (status & OHCI_INTR_UE) {
      klog_puts("[OHCI] Unrecoverable Error!\n");
    }
  }
}

// Controller Initialization

static void ohci_silence(struct ohci_controller *hc) {
  uint32_t control = ohci_read32(hc, OHCI_REG_CONTROL);
  if ((control & OHCI_CTRL_HCFS_MASK) != OHCI_CTRL_HCFS_RESET) {
    klog_puts("[OHCI] Stopping running controller...\n");
    ohci_write32(hc, OHCI_REG_INTERRUPT_DISABLE, OHCI_INTR_MIE);
    ohci_write32(hc, OHCI_REG_CONTROL, OHCI_CTRL_HCFS_RESET);
  }
  ohci_write32(hc, OHCI_REG_INTERRUPT_STATUS, 0xFFFFFFFF);
}

static bool ohci_probe_pci_device(struct pci_device *pci) {
  if (controller_count >= OHCI_MAX_CONTROLLERS)
    return false;

  uint64_t mmio_phys = pci->bar[0] & ~0xF;
  if (mmio_phys == 0)
    return false;

  struct ohci_controller *hc = &controllers[controller_count];
  hc->mmio_base = (uintptr_t)PHYS_TO_VIRT(mmio_phys);
  hc->irq_line = pci->irq_line;
  hc->pci_bus = pci->bus;
  hc->pci_slot = pci->slot;
  hc->pci_func = pci->func;
  hc->vendor_id = pci->vendor_id;
  hc->device_id = pci->device_id;
  hc->present = true;

  // The controller BAR may be outside the physical span covered by the HHDM.
  // Explicitly install fresh MMIO mappings; fresh PTEs do not need a TLB
  // shootdown.
  vmm_map_page(vmm_get_active_pml4(), hc->mmio_base, mmio_phys,
               PAGE_FLAG_RW | PAGE_FLAG_PRESENT);
  vmm_map_page(vmm_get_active_pml4(), hc->mmio_base + 0x1000,
               mmio_phys + 0x1000, PAGE_FLAG_RW | PAGE_FLAG_PRESENT);

  ohci_silence(hc);

  uint32_t rev = ohci_read32(hc, OHCI_REG_REVISION);
  uint32_t descA = ohci_read32(hc, OHCI_REG_RH_DESCRIPTOR_A);
  hc->num_ports = descA & OHCI_RHA_NDP_MASK;

  klog_puts("[OHCI] Initializing Controller: Rev=0x");
  klog_hex32(rev & 0xFF);
  klog_puts(", Ports=");
  klog_uint64(hc->num_ports);
  klog_puts(", IRQ=");
  klog_uint64(hc->irq_line);
  klog_puts("\n");

  // DMA Resource Allocation
  uint64_t phys;
  hc->hcca = (struct ohci_hcca *)dma_alloc_page(&phys);
  hc->hcca_phys = (uint32_t)phys;
  hc->ed_pool = (struct ohci_ed *)dma_alloc_page(&phys);
  hc->ed_pool_phys = (uint32_t)phys;
  hc->td_pool = (struct ohci_td *)dma_alloc_page(&phys);
  hc->td_pool_phys = (uint32_t)phys;
  hc->transfer_buffer = dma_alloc_page(&phys);
  hc->transfer_buffer_phys = (uint32_t)phys;

  // Zero all DMA structures
  uint8_t *p = (uint8_t *)hc->hcca;
  for (int i = 0; i < 4096; i++)
    p[i] = 0;
  p = (uint8_t *)hc->ed_pool;
  for (int i = 0; i < 4096; i++)
    p[i] = 0;
  p = (uint8_t *)hc->td_pool;
  for (int i = 0; i < 4096; i++)
    p[i] = 0;
  p = (uint8_t *)hc->transfer_buffer;
  for (int i = 0; i < 4096; i++)
    p[i] = 0;

  // Initialize pool counters
  hc->int_ed_used = 0;
  hc->int_td_used = 0;
  hc->ed_used = OHCI_CTRL_ED_START;
  hc->td_used = OHCI_CTRL_TD_START;

  // Reset Sequence
  ohci_write32(hc, OHCI_REG_COMMAND_STATUS, OHCI_CMD_HCR);
  for (int i = 0; i < 10000; i++)
    io_wait();

  // Configuration
  ohci_write32(hc, OHCI_REG_FM_INTERVAL,
               0x2edf | ((((0x2edf - 210) * 6) / 7) << 16));
  ohci_write32(hc, OHCI_REG_PERIODIC_START, (0x2edf * 9) / 10);
  ohci_write32(hc, OHCI_REG_HCCA, hc->hcca_phys);
  ohci_write32(hc, OHCI_REG_CONTROL_HEAD_ED, 0);
  ohci_write32(hc, OHCI_REG_BULK_HEAD_ED, 0);

  // Hook Interrupts
  hc->irq_registered =
      irq_install_handler(hc->irq_line, ohci_irq_handler, 0x000F);
  if (hc->irq_registered) {
    ohci_write32(hc, OHCI_REG_INTERRUPT_ENABLE,
                 OHCI_INTR_MIE | OHCI_INTR_WDH | OHCI_INTR_UE | OHCI_INTR_RHSC);
  }

  // GO!
  uint32_t control = ohci_read32(hc, OHCI_REG_CONTROL);
  control &= ~OHCI_CTRL_HCFS_MASK;
  control |= OHCI_CTRL_HCFS_OPERATIONAL;
  ohci_write32(hc, OHCI_REG_CONTROL, control);

  // Register as HCD
  hc->hcd.priv = hc;
  hc->hcd.name = "ohci";
  hc->hcd.control_transfer = ohci_hcd_control_transfer;
  hc->hcd.address_device = NULL;
  hc->hcd.device_removed = NULL;

  // Enable PCI Bus Mastering
  pci_config_write32(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04, 0x07);

  // Power root hub ports
  if (descA & OHCI_RHA_NPS) {
    // No power switching — always powered
  } else {
    ohci_write32(hc, OHCI_REG_RH_STATUS, (1 << 16)); // SetGlobalPower
    for (int i = 0; i < 50000; i++)
      io_wait();
  }

  klog_puts("[OHCI] Controller Operational.\n");
  controller_count++;

  // Probe root hub ports
  for (int i = 0; i < 100000; i++)
    io_wait();
  ohci_probe_ports(hc);

  return true;
}

// Public API

void ohci_init(void) {
  controller_count = 0;
  int_pipe_count = 0;
  klog_puts("[OHCI] Initializing controllers...\n");

  uint32_t pci_count = pci_get_device_count();
  for (uint32_t i = 0; i < pci_count; i++) {
    struct pci_device *dev = pci_get_device(i);
    if (!dev)
      continue;
    if (dev->class_code == 0x0C && dev->subclass == 0x03 &&
        dev->prog_if == 0x10) {
      ohci_probe_pci_device(dev);
    }
  }
}

int ohci_get_controller_count(void) { return controller_count; }

struct ohci_controller *ohci_get_controller(int index) {
  if (index < 0 || index >= controller_count)
    return NULL;
  return &controllers[index];
}

#endif
