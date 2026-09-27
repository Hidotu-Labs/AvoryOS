#include "xhci.h"
#include "../../apic/lapic.h"
#include "../../apic/lapic_timer.h"
#include "../../console/klog.h"
#include "../../cpu/isr.h"
#include "../../io/io.h"
#include "../../lib/string.h"
#include "../../mm/dma_alloc.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "usb.h"
#include "usb_kbd.h"
#include "usb_mouse.h"
#include <stddef.h>

#define XHCI_USBCMD 0x00
#define XHCI_USBSTS 0x04
#define XHCI_PAGESIZE 0x08
#define XHCI_CRCR 0x18
#define XHCI_DCBAAP 0x30
#define XHCI_CONFIG 0x38
#define XHCI_CMD_RUN (1U << 0)
#define XHCI_CMD_RESET (1U << 1)
#define XHCI_CMD_INTE (1U << 2)
#define XHCI_STS_HALTED (1U << 0)
#define XHCI_STS_CNR (1U << 11)
#define XHCI_IMAN_IP (1U << 0)
#define XHCI_IMAN_IE (1U << 1)
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_TYPE(n) ((uint32_t)(n) << 10)
#define XHCI_TRB_LINK 6U
#define XHCI_TRB_ENABLE_SLOT 9U
#define XHCI_TRB_DISABLE_SLOT 10U
#define XHCI_TRB_ADDRESS_DEVICE 11U
#define XHCI_TRB_CONFIGURE_ENDPOINT 12U
#define XHCI_TRB_EVALUATE_CONTEXT 13U
#define XHCI_TRB_RESET_ENDPOINT 14U
#define XHCI_TRB_STOP_ENDPOINT 15U
#define XHCI_TRB_SET_TR_DEQUEUE 16U
#define XHCI_TRB_NOOP_CMD 23U
#define XHCI_TRB_NORMAL 1U
#define XHCI_TRB_TRANSFER_EVENT 32U
#define XHCI_TRB_COMMAND_COMPLETION 33U
#define XHCI_TRB_PORT_STATUS 34U
#define XHCI_TRB_SETUP_STAGE 2U
#define XHCI_TRB_DATA_STAGE 3U
#define XHCI_TRB_STATUS_STAGE 4U
#define XHCI_TRB_IOC (1U << 5)
#define XHCI_TRB_IDT (1U << 6)
#define XHCI_TRB_CHAIN (1U << 4)
#define XHCI_TRB_ISP (1U << 2)
#define XHCI_TRB_DIR_IN (1U << 16)
#define XHCI_PORTSC 0x400
#define XHCI_PORT_CCS (1U << 0)
#define XHCI_PORT_PED (1U << 1)
#define XHCI_PORT_PR (1U << 4)
#define XHCI_PORT_PP (1U << 9)
#define XHCI_PORT_SPEED(s) (((s) >> 10) & 0xFU)
#define XHCI_PORT_WPR (1U << 31)
#define XHCI_PORT_CHANGE_MASK (0x7FU << 17)
#define XHCI_TRB_TYPE_GET(c) (((c) >> 10) & 0x3FU)
#define XHCI_COMPLETION_GET(s) (((s) >> 24) & 0xFFU)
#define XHCI_COMPLETION_SUCCESS 1U
#define XHCI_WAIT_LOOPS 5000000U

static struct xhci_controller controllers[XHCI_MAX_CONTROLLERS];
static int controller_count;
static int matched_count;
static const char *last_probe_failure = "none";
static const char *last_dma_object = "none";
static bool last_ac64;
static const char *last_dma_layer = "none";
static uint32_t last_dma_flags;
static uint64_t last_dma_phys;

static inline uint32_t mmio_read32(volatile void *base, uint32_t offset) {
  return *(volatile uint32_t *)((volatile uint8_t *)base + offset);
}

static inline void mmio_write32(volatile void *base, uint32_t offset,
                                uint32_t value) {
  *(volatile uint32_t *)((volatile uint8_t *)base + offset) = value;
}

static inline void mmio_write64(volatile void *base, uint32_t offset,
                                uint64_t value) {
  *(volatile uint64_t *)((volatile uint8_t *)base + offset) = value;
}

static uint64_t xhci_bar0(struct pci_device *pci) {
  if (!pci || (pci->bar[0] & 1U))
    return 0;
  uint64_t result = pci->bar[0] & ~0xFULL;
  if (((pci->bar[0] >> 1) & 3U) == 2U)
    result |= (uint64_t)pci->bar[1] << 32;
  return result;
}

static bool xhci_map_mmio(uint64_t phys, uint64_t length) {
  uint64_t first = phys & ~0xFFFULL;
  uint64_t last = (phys + length - 1) & ~0xFFFULL;
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_PCD |
                   PAGE_FLAG_PWT;
  for (uint64_t page = first; page <= last; page += 4096) {
    uint64_t virt = page + hhdm;
    if (!vmm_virt_to_phys(pml4, virt) &&
        !vmm_map_page(pml4, virt, page, flags))
      return false;
    vmm_flush_tlb(virt);
  }
  return true;
}

static bool xhci_wait32(volatile void *base, uint32_t offset, uint32_t mask,
                        uint32_t expected) {
  for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
    if ((mmio_read32(base, offset) & mask) == expected)
      return true;
    io_wait();
  }
  return false;
}

/* USB timing requirements are wall-clock requirements. A loop count based on
   io_wait() is much shorter on physical machines than it is under emulation. */
static void xhci_delay_ms(uint32_t ms) {
  uint64_t deadline = lapic_timer_get_ms() + ms;
  while (lapic_timer_get_ms() < deadline)
    io_wait();
}

static void xhci_legacy_handoff(struct xhci_controller *hc,
                                uint32_t hccparams1) {
  uint32_t offset = ((hccparams1 >> 16) & 0xFFFFU) * 4U;
  for (uint32_t guard = 0; offset && guard < 64; guard++) {
    uint32_t cap = mmio_read32(hc->cap, offset);
    uint8_t id = cap & 0xFFU;
    uint32_t next = ((cap >> 8) & 0xFFU) * 4U;
    if (id == 1) {
      mmio_write32(hc->cap, offset, cap | (1U << 24));
      for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
        if (!(mmio_read32(hc->cap, offset) & (1U << 16)))
          break;
        io_wait();
      }
      mmio_write32(hc->cap, offset + 4, 0);
      return;
    }
    if (!next)
      return;
    /* xECP Next is an absolute DWORD offset from the capability base, not an
       offset relative to the current extended capability. */
    offset = next;
  }
}

static bool xhci_halt_reset(struct xhci_controller *hc) {
  uint32_t cmd = mmio_read32(hc->op, XHCI_USBCMD);
  mmio_write32(hc->op, XHCI_USBCMD, cmd & ~XHCI_CMD_RUN);
  if (!xhci_wait32(hc->op, XHCI_USBSTS, XHCI_STS_HALTED,
                   XHCI_STS_HALTED)) {
    hc->timeouts++;
    return false;
  }
  mmio_write32(hc->op, XHCI_USBCMD, XHCI_CMD_RESET);
  if (!xhci_wait32(hc->op, XHCI_USBCMD, XHCI_CMD_RESET, 0) ||
      !xhci_wait32(hc->op, XHCI_USBSTS, XHCI_STS_CNR, 0)) {
    hc->timeouts++;
    return false;
  }
  hc->running = false;
  return true;
}

static bool xhci_alloc_page(struct xhci_controller *hc, void **virt,
                            uint64_t *phys) {
  uint32_t flags = hc->ac64 ? DMA_FLAG_ANYWHERE : DMA_FLAG_32BIT;
  *virt = dma_alloc_page_flags(flags, phys);
  return *virt != NULL && ((*phys & 0xFFFU) == 0);
}

static bool xhci_alloc_pages(struct xhci_controller *hc, size_t count,
                             void **virt, uint64_t *phys) {
  uint32_t flags = hc->ac64 ? DMA_FLAG_ANYWHERE : DMA_FLAG_32BIT;
  *virt = dma_alloc_pages_flags(count, flags, phys);
  return *virt != NULL && ((*phys & 0xFFFU) == 0);
}

static bool xhci_alloc_runtime(struct xhci_controller *hc) {
  last_dma_object = "DCBAA";
  if (!xhci_alloc_page(hc, &hc->dcbaa, &hc->dcbaa_phys))
    return false;
  last_dma_object = "command ring";
  if (!xhci_alloc_page(hc, (void **)&hc->command_ring,
                       &hc->command_ring_phys))
    return false;
  last_dma_object = "event ring";
  if (!xhci_alloc_page(hc, (void **)&hc->event_ring, &hc->event_ring_phys))
    return false;
  last_dma_object = "ERST";
  if (!xhci_alloc_page(hc, (void **)&hc->erst, &hc->erst_phys))
    return false;

  if (hc->scratchpad_count) {
    last_dma_object = "scratchpad array";
    size_t array_pages =
        ((size_t)hc->scratchpad_count * sizeof(uint64_t) + 4095U) / 4096U;
    if (!xhci_alloc_pages(hc, array_pages, &hc->scratchpad_array,
                          &hc->scratchpad_array_phys))
      return false;
    uint64_t *array = (uint64_t *)hc->scratchpad_array;
    for (uint16_t i = 0; i < hc->scratchpad_count; i++) {
      last_dma_object = "scratchpad page";
      void *scratchpad;
      uint64_t scratchpad_phys;
      if (!xhci_alloc_page(hc, &scratchpad, &scratchpad_phys))
        return false;
      array[i] = scratchpad_phys;
    }
    ((uint64_t *)hc->dcbaa)[0] = hc->scratchpad_array_phys;
  }

  hc->command_cycle = 1;
  hc->event_cycle = 1;
  hc->command_ring[XHCI_RING_TRBS - 1].parameter = hc->command_ring_phys;
  hc->command_ring[XHCI_RING_TRBS - 1].control =
      XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_CYCLE | (1U << 1);
  hc->erst[0].base = hc->event_ring_phys;
  hc->erst[0].size = XHCI_RING_TRBS;
  last_dma_object = "none";
  return true;
}

static void xhci_program_runtime(struct xhci_controller *hc) {
  mmio_write64(hc->op, XHCI_DCBAAP, hc->dcbaa_phys);
  mmio_write64(hc->op, XHCI_CRCR, hc->command_ring_phys | hc->command_cycle);
  mmio_write32(hc->op, XHCI_CONFIG, hc->max_slots);
  volatile uint8_t *ir = hc->runtime + 0x20;
  mmio_write32(ir, 0x08, 1);
  mmio_write64(ir, 0x10, hc->erst_phys);
  mmio_write64(ir, 0x18, hc->event_ring_phys);
  mmio_write32(ir, 0x04, 0);
  mmio_write32(ir, 0x00, XHCI_IMAN_IP | XHCI_IMAN_IE);
}

static bool xhci_start(struct xhci_controller *hc) {
  xhci_program_runtime(hc);
  mmio_write32(hc->op, XHCI_USBCMD, XHCI_CMD_RUN | XHCI_CMD_INTE);
  if (!xhci_wait32(hc->op, XHCI_USBSTS, XHCI_STS_HALTED, 0)) {
    hc->timeouts++;
    return false;
  }
  hc->running = true;
  return true;
}

static struct xhci_trb *xhci_event_peek(struct xhci_controller *hc) {
  struct xhci_trb *event = &hc->event_ring[hc->event_dequeue];
  if ((event->control & XHCI_TRB_CYCLE) != hc->event_cycle)
    return NULL;
  return event;
}

static void xhci_event_advance(struct xhci_controller *hc) {
  hc->event_dequeue++;
  if (hc->event_dequeue == XHCI_RING_TRBS) {
    hc->event_dequeue = 0;
    hc->event_cycle ^= 1;
    hc->ring_wraps++;
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);
  mmio_write64(hc->runtime + 0x20, 0x18,
               hc->event_ring_phys +
                   hc->event_dequeue * sizeof(struct xhci_trb) +
                   (1U << 3));
}

static uint32_t xhci_drain_events(struct xhci_controller *hc,
                                  uint64_t awaited_command) {
  uint32_t completion = 0;
  struct xhci_trb *event;
  while ((event = xhci_event_peek(hc)) != NULL) {
    hc->events_seen++;
    if (XHCI_TRB_TYPE_GET(event->control) == XHCI_TRB_COMMAND_COMPLETION) {
      hc->commands_completed++;
      if ((event->parameter & ~0xFULL) == awaited_command)
        completion = XHCI_COMPLETION_GET(event->status);
      hc->last_command_trb = event->parameter & ~0xFULL;
      hc->last_completion_code = XHCI_COMPLETION_GET(event->status);
      hc->last_command_slot = (event->control >> 24) & 0xFFU;
    } else if (XHCI_TRB_TYPE_GET(event->control) == XHCI_TRB_TRANSFER_EVENT) {
      hc->last_transfer_trb = event->parameter & ~0xFULL;
      hc->last_transfer_code = XHCI_COMPLETION_GET(event->status);
      for (uint32_t i = 0; i < XHCI_MAX_INTERRUPT_PIPES; i++) {
        struct xhci_interrupt_state *state = &hc->interrupt_pipes[i];
        if (!state->pipe.active ||
            state->expected_trb != (event->parameter & ~0xFULL))
          continue;
        state->completion_code = XHCI_COMPLETION_GET(event->status);
        {
          uint32_t residual = event->status & 0xFFFFFFU;
          state->pipe.actual_length =
              residual < state->pipe.buffer_len
                  ? (uint16_t)(state->pipe.buffer_len - residual)
                  : 0;
        }
        state->completed = true;
        state->completions++;
        break;
      }
    } else if (XHCI_TRB_TYPE_GET(event->control) == XHCI_TRB_PORT_STATUS) {
      uint8_t port = (event->parameter >> 24) & 0xFFU;
      if (port && port <= 32)
        hc->pending_ports |= 1U << (port - 1);
    }
    xhci_event_advance(hc);
  }
  return completion;
}

static bool xhci_submit_command(struct xhci_controller *hc, uint64_t parameter,
                                uint32_t status, uint32_t control,
                                uint8_t *slot_id) {
  uint16_t index = hc->command_enqueue;
  struct xhci_trb *trb = &hc->command_ring[index];
  uint64_t phys = hc->command_ring_phys + index * sizeof(*trb);
  hc->debug_stage = "command submitted";
  hc->debug_last_command_type = XHCI_TRB_TYPE_GET(control);
  trb->parameter = parameter;
  trb->status = status;
  trb->control = control | hc->command_cycle;
  hc->command_enqueue++;
  if (hc->command_enqueue == XHCI_RING_TRBS - 1) {
    hc->command_ring[XHCI_RING_TRBS - 1].control =
        XHCI_TRB_TYPE(XHCI_TRB_LINK) | hc->command_cycle | (1U << 1);
    hc->command_enqueue = 0;
    hc->command_cycle ^= 1;
    hc->ring_wraps++;
  }
  hc->commands_submitted++;
  hc->last_command_trb = 0;
  hc->last_completion_code = 0;
  hc->last_command_slot = 0;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  hc->doorbells[0] = 0;
  for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
    uint32_t cc = xhci_drain_events(hc, phys);
    if (!cc && hc->last_command_trb == phys)
      cc = hc->last_completion_code;
    if (cc) {
      if (slot_id)
        *slot_id = hc->last_command_slot;
      hc->debug_last_slot = hc->last_command_slot;
      hc->debug_stage = cc == XHCI_COMPLETION_SUCCESS ? "command completed"
                                                     : "command failed";
      if (cc != XHCI_COMPLETION_SUCCESS) {
        klog_puts("[XHCI] Command failed: type=");
        klog_uint64(XHCI_TRB_TYPE_GET(control));
        klog_puts(" completion=");
        klog_uint64(cc);
        klog_puts("\n");
      }
      return cc == XHCI_COMPLETION_SUCCESS;
    }
    io_wait();
  }
  hc->timeouts++;
  hc->debug_stage = "command timeout";
  hc->debug_usbcmd = mmio_read32(hc->op, XHCI_USBCMD);
  hc->debug_usbsts = mmio_read32(hc->op, XHCI_USBSTS);
  klog_puts("[XHCI] Command timeout: USBSTS=0x");
  klog_hex32(mmio_read32(hc->op, XHCI_USBSTS));
  klog_puts(" enqueue=");
  klog_uint64(index);
  klog_puts(" event_dequeue=");
  klog_uint64(hc->event_dequeue);
  klog_puts("\n");
  return false;
}

static bool xhci_noop_command(struct xhci_controller *hc) {
  return xhci_submit_command(hc, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_NOOP_CMD),
                             NULL);
}

static uint32_t *xhci_context(void *base, uint8_t index, bool context_64) {
  return (uint32_t *)((uint8_t *)base + index * (context_64 ? 64U : 32U));
}

static enum usb_speed xhci_usb_speed(uint8_t speed_id) {
  switch (speed_id) {
  case 1: return USB_SPEED_FULL;
  case 2: return USB_SPEED_LOW;
  case 3: return USB_SPEED_HIGH;
  case 4: return USB_SPEED_SUPER;
  case 5: return USB_SPEED_SUPER_PLUS;
  default: return USB_SPEED_UNKNOWN;
  }
}

static uint16_t xhci_initial_mps(enum usb_speed speed) {
  if (speed == USB_SPEED_SUPER || speed == USB_SPEED_SUPER_PLUS)
    return 512;
  if (speed == USB_SPEED_HIGH)
    return 64;
  return 8;
}

static bool xhci_slot_alloc(struct xhci_controller *hc,
                            struct xhci_slot *slot) {
  if (!xhci_alloc_page(hc, &slot->output_context,
                       &slot->output_context_phys) ||
      !xhci_alloc_page(hc, &slot->input_context, &slot->input_context_phys) ||
      !xhci_alloc_page(hc, (void **)&slot->ep0_ring, &slot->ep0_ring_phys) ||
      !xhci_alloc_page(hc, &slot->control_buffer, &slot->control_buffer_phys))
    return false;
  slot->ep0_cycle = 1;
  slot->ep0_ring[XHCI_RING_TRBS - 1].parameter = slot->ep0_ring_phys;
  slot->ep0_ring[XHCI_RING_TRBS - 1].control =
      XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_CYCLE | (1U << 1);
  return true;
}

static void xhci_build_address_context(struct xhci_controller *hc,
                                       struct xhci_slot *slot) {
  memset(slot->input_context, 0, 4096);
  uint32_t *icc = xhci_context(slot->input_context, 0, hc->context_64);
  uint32_t *sc = xhci_context(slot->input_context, 1, hc->context_64);
  uint32_t *ep0 = xhci_context(slot->input_context, 2, hc->context_64);
  icc[1] = 3; // Add Slot Context and Endpoint Context 0.
  uint32_t portsc = mmio_read32(hc->op, XHCI_PORTSC + slot->port * 0x10U);
  uint8_t speed_id = XHCI_PORT_SPEED(portsc);
  sc[0] = (1U << 27) | ((uint32_t)speed_id << 20);
  sc[1] = ((uint32_t)slot->port + 1U) << 16;
  ep0[1] = (3U << 1) | (4U << 3) |
           ((uint32_t)slot->ep0_max_packet << 16);
  uint64_t dequeue = slot->ep0_ring_phys +
                     slot->ep0_enqueue * sizeof(struct xhci_trb);
  dequeue |= slot->ep0_cycle;
  ep0[2] = (uint32_t)dequeue;
  ep0[3] = (uint32_t)(dequeue >> 32);
  ep0[4] = 8;
}

static int xhci_prepare_device(struct usb_hcd *hcd, struct usb_device *dev) {
  struct xhci_controller *hc = hcd->priv;
  hc->debug_stage = "enable slot";
  hc->debug_last_port = dev->port;
  uint8_t slot_id = 0;
  if (!xhci_submit_command(hc, 0, 0,
                           XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), &slot_id) ||
      !slot_id || slot_id > hc->max_slots) {
    klog_puts("[XHCI] Enable Slot failed\n");
    return -1;
  }
  struct xhci_slot *slot = &hc->slots[slot_id];
  memset(slot, 0, sizeof(*slot));
  slot->id = slot_id;
  slot->port = dev->port;
  slot->speed = dev->speed;
  slot->ep0_max_packet = xhci_initial_mps(dev->speed);
  if (!xhci_slot_alloc(hc, slot)) {
    klog_puts("[XHCI] Slot DMA allocation failed\n");
    return -1;
  }
  ((uint64_t *)hc->dcbaa)[slot_id] = slot->output_context_phys;
  hc->debug_stage = "address device";
  xhci_build_address_context(hc, slot);
  if (!xhci_submit_command(
          hc, slot->input_context_phys, 0,
          XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) |
              ((uint32_t)slot_id << 24),
          NULL)) {
    klog_puts("[XHCI] Address Device failed\n");
    return -1;
  }
  slot->enabled = true;
  dev->hcd_data = slot;
  uint32_t *out_slot = xhci_context(slot->output_context, 0, hc->context_64);
  dev->address = out_slot[3] & 0xFFU;
  if (!dev->address) {
    hc->debug_stage = "Address Device returned address zero";
    return -1;
  }
  /* USB 2.0 requires a recovery interval after SET_ADDRESS before the next
     request. Address Device performs that request on behalf of software. */
  xhci_delay_ms(10);
  hc->debug_stage = "device addressed";
  return 0;
}

static int xhci_address_device(struct usb_hcd *hcd, struct usb_device *dev,
                               uint8_t requested_address) {
  (void)requested_address; /* xHC assigned the address in device_prepare. */
  struct xhci_controller *hc = hcd->priv;
  struct xhci_slot *slot = dev->hcd_data;
  if (!slot || !slot->enabled)
    return -1;

  /* After the first eight descriptor bytes, tell the xHC the device's real
     EP0 maximum packet size. Address Device must not be issued a second time
     for an already-addressed slot; Evaluate Context is the specified update. */
  uint16_t mps = dev->desc.max_packet_size;
  if (dev->speed == USB_SPEED_SUPER ||
      dev->speed == USB_SPEED_SUPER_PLUS)
    mps = (mps < 16) ? (uint16_t)(1U << mps) : mps;
  if (!mps)
    return -1;
  slot->ep0_max_packet = mps;
  memset(slot->input_context, 0, 4096);
  uint32_t *icc = xhci_context(slot->input_context, 0, hc->context_64);
  uint32_t *input_ep0 = xhci_context(slot->input_context, 2,
                                     hc->context_64);
  uint32_t *output_ep0 = xhci_context(slot->output_context, 1,
                                      hc->context_64);
  icc[1] = 1U << 1; /* Add endpoint context DCI 1 only. */
  memcpy(input_ep0, output_ep0, hc->context_64 ? 64U : 32U);
  input_ep0[1] = (input_ep0[1] & 0x0000FFFFU) | ((uint32_t)mps << 16);
  hc->debug_stage = "evaluate EP0 context";
  if (!xhci_submit_command(
          hc, slot->input_context_phys, 0,
          XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CONTEXT) |
              ((uint32_t)slot->id << 24),
          NULL)) {
    klog_puts("[XHCI] Evaluate EP0 Context failed\n");
    return -1;
  }
  hc->debug_stage = "EP0 context updated";
  return 0;
}

static uint64_t xhci_ep0_enqueue(struct xhci_slot *slot, uint64_t parameter,
                                 uint32_t status, uint32_t control,
                                 bool defer_ownership) {
  uint16_t index = slot->ep0_enqueue;
  struct xhci_trb *trb = &slot->ep0_ring[index];
  trb->parameter = parameter;
  trb->status = status;
  trb->control = control | (defer_ownership ? (slot->ep0_cycle ^ 1U)
                                             : slot->ep0_cycle);
  uint64_t phys = slot->ep0_ring_phys + index * sizeof(*trb);
  slot->ep0_enqueue++;
  if (slot->ep0_enqueue == XHCI_RING_TRBS - 1) {
    slot->ep0_ring[XHCI_RING_TRBS - 1].control =
        XHCI_TRB_TYPE(XHCI_TRB_LINK) | slot->ep0_cycle | (1U << 1);
    slot->ep0_enqueue = 0;
    slot->ep0_cycle ^= 1;
  }
  return phys;
}

static bool xhci_recover_ep0(struct xhci_controller *hc,
                             struct xhci_slot *slot) {
  hc->debug_stage = "reset halted EP0";
  uint32_t endpoint = 1U << 16; /* DCI 1 is the default control endpoint. */
  uint32_t slot_id = (uint32_t)slot->id << 24;
  if (!xhci_submit_command(hc, 0, 0,
                           XHCI_TRB_TYPE(XHCI_TRB_RESET_ENDPOINT) |
                               endpoint | slot_id,
                           NULL))
    return false;

  /* Skip every TRB left in the failed control TD. Reset Endpoint deliberately
     preserves the old dequeue pointer, so Set TR Dequeue is required before a
     retry can make progress. */
  uint64_t dequeue = slot->ep0_ring_phys +
                     slot->ep0_enqueue * sizeof(struct xhci_trb);
  dequeue |= slot->ep0_cycle;
  hc->debug_stage = "set EP0 dequeue";
  return xhci_submit_command(hc, dequeue, 0,
                             XHCI_TRB_TYPE(XHCI_TRB_SET_TR_DEQUEUE) |
                                 endpoint | slot_id,
                             NULL);
}

static int xhci_control_device(struct usb_hcd *hcd, struct usb_device *dev,
                               struct usb_control_request *req, void *data,
                               uint16_t len) {
  struct xhci_controller *hc = hcd->priv;
  struct xhci_slot *slot = dev->hcd_data;
  if (!slot || !slot->enabled || !req || len > 4096)
    return -1;
  uint64_t setup = 0;
  memcpy(&setup, req, sizeof(*req));
  bool in = (req->request_type & 0x80U) != 0;
  hc->debug_stage = "EP0 transfer";
  hc->debug_last_ep0_request = req->request;
  hc->debug_last_ep0_value = req->value;
  hc->debug_last_ep0_length = len;
  uint8_t attempt = 0;

retry:
  attempt++;
  hc->debug_stage = attempt == 1 ? "EP0 transfer" : "EP0 retry";
  if (in && len)
    memset(slot->control_buffer, 0, len);
  uint32_t trt = len ? (in ? 3U : 2U) : 0U;
  uint16_t setup_index = slot->ep0_enqueue;
  uint8_t setup_cycle = slot->ep0_cycle;
  xhci_ep0_enqueue(slot, setup, 8,
                   XHCI_TRB_TYPE(XHCI_TRB_SETUP_STAGE) | XHCI_TRB_IDT |
                       (trt << 16),
                   true);
  if (len) {
    if (!in && data)
      memcpy(slot->control_buffer, data, len);
    xhci_ep0_enqueue(slot, slot->control_buffer_phys, len,
                     XHCI_TRB_TYPE(XHCI_TRB_DATA_STAGE) |
                         (in ? XHCI_TRB_DIR_IN : 0),
                     false);
  }
  uint64_t status_trb = xhci_ep0_enqueue(
      slot, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_STATUS_STAGE) | XHCI_TRB_IOC |
                      ((!len || !in) ? XHCI_TRB_DIR_IN : 0),
      false);
  hc->last_transfer_trb = 0;
  hc->last_transfer_code = 0;
  /* Publish the complete control TD atomically. The Setup TRB was written
     with the inverse cycle bit so a fast physical xHC could not consume it
     while its Data and Status stages were still being constructed. */
  __atomic_thread_fence(__ATOMIC_RELEASE);
  slot->ep0_ring[setup_index].control =
      (slot->ep0_ring[setup_index].control & ~XHCI_TRB_CYCLE) | setup_cycle;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  hc->doorbells[slot->id] = 1;
  for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
    xhci_drain_events(hc, 0);
    /* An error is reported against the Setup or Data TRB, not necessarily the
       IOC Status TRB. Waiting only for status turns a prompt transaction error
       into a misleading timeout. */
    if (hc->last_transfer_trb &&
        hc->last_transfer_code != XHCI_COMPLETION_SUCCESS &&
        hc->last_transfer_code != 13) {
      uint8_t cc = hc->last_transfer_code;
      hc->debug_stage = "EP0 transaction error";
      klog_puts("[XHCI] EP0 TD failed before status: completion=");
      klog_uint64(cc);
      klog_puts(" attempt=");
      klog_uint64(attempt);
      klog_puts("\n");
      if (attempt < 3 && xhci_recover_ep0(hc, slot))
        goto retry;
      return -1;
    }
    if (hc->last_transfer_trb == status_trb) {
      uint8_t cc = hc->last_transfer_code;
      if (cc != XHCI_COMPLETION_SUCCESS && cc != 13) {
        hc->debug_stage = "EP0 completion error";
        klog_puts("[XHCI] EP0 transfer failed: completion=");
        klog_uint64(cc);
        klog_puts("\n");
        return -1;
      }
      if (in && data && len)
      {
        /* Do not allow the compiler or CPU to move payload reads ahead of the
           event-ring completion observed above. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (uint8_t n = 0; n < 8 && n < len; n++)
          hc->debug_ep0_data[n] = ((uint8_t *)slot->control_buffer)[n];

        /* A successful Status Stage is not useful if a controller/device
           returned no descriptor payload. Retry invalid descriptor headers;
           this also handles devices needing a little more address recovery. */
        if (req->request == USB_REQ_GET_DESCRIPTOR && len >= 2 &&
            (((uint8_t *)slot->control_buffer)[0] < 2 ||
             ((uint8_t *)slot->control_buffer)[1] != (req->value >> 8))) {
          hc->debug_stage = "invalid descriptor payload";
          klog_puts("[XHCI] Invalid descriptor payload, retrying\n");
          if (attempt < 3) {
            xhci_delay_ms(10);
            goto retry;
          }
          return -1;
        }
        memcpy(data, slot->control_buffer, len);
      }
      hc->debug_stage = "EP0 completed";
      return 0;
    }
    io_wait();
  }
  hc->timeouts++;
  hc->debug_stage = "EP0 timeout";
  hc->debug_usbcmd = mmio_read32(hc->op, XHCI_USBCMD);
  hc->debug_usbsts = mmio_read32(hc->op, XHCI_USBSTS);
  klog_puts("[XHCI] EP0 transfer timed out\n");
  return -1;
}

static uint32_t xhci_port_neutral(uint32_t portsc) {
  return portsc & (XHCI_PORT_CCS | (1U << 3) | (0xFU << 10) |
                   XHCI_PORT_PP | (3U << 14) | (7U << 25));
}

static bool xhci_reset_port(struct xhci_controller *hc, uint8_t port) {
  uint32_t offset = XHCI_PORTSC + port * 0x10U;
  uint32_t ps = mmio_read32(hc->op, offset);
  hc->debug_stage = "port reset begin";
  hc->debug_last_port = port;
  hc->debug_portsc[port] = ps;
  if (!(ps & XHCI_PORT_CCS))
    return false;
  if (!(ps & XHCI_PORT_PP)) {
    mmio_write32(hc->op, offset, xhci_port_neutral(ps) | XHCI_PORT_PP);
    /* Allow root-port power and an attached device to become stable. */
    xhci_delay_ms(100);
    ps = mmio_read32(hc->op, offset);
    hc->debug_portsc[port] = ps;
    if (!(ps & XHCI_PORT_CCS))
      return false;
  }
  uint8_t speed_id = XHCI_PORT_SPEED(ps);
  uint32_t reset = speed_id >= 4 ? XHCI_PORT_WPR : XHCI_PORT_PR;
  mmio_write32(hc->op, offset, xhci_port_neutral(ps) | reset);
  for (uint32_t i = 0; i < XHCI_WAIT_LOOPS; i++) {
    ps = mmio_read32(hc->op, offset);
    if (!(ps & reset) && (ps & XHCI_PORT_PED)) {
      mmio_write32(hc->op, offset,
                   xhci_port_neutral(ps) | XHCI_PORT_CHANGE_MASK);
      /* USB 2 devices require reset recovery before the first request. */
      xhci_delay_ms(10);
      hc->debug_portsc[port] = mmio_read32(hc->op, offset);
      hc->debug_port_result[port] = 2;
      hc->debug_stage = "port reset complete";
      return true;
    }
    io_wait();
  }
  hc->debug_portsc[port] = ps;
  hc->debug_port_result[port] = 3;
  hc->debug_stage = "port reset timeout";
  return false;
}

static void xhci_enumerate_ports(struct xhci_controller *hc) {
  for (uint8_t port = 0; port < hc->max_ports; port++) {
    uint32_t ps = mmio_read32(hc->op, XHCI_PORTSC + port * 0x10U);
    hc->debug_portsc[port] = ps;
    hc->debug_port_result[port] = (ps & XHCI_PORT_CCS) ? 1 : 0;
    if (!(ps & XHCI_PORT_CCS))
      continue;
    enum usb_speed speed = xhci_usb_speed(XHCI_PORT_SPEED(ps));
    if (speed == USB_SPEED_UNKNOWN || !xhci_reset_port(hc, port)) {
      klog_puts("[XHCI] Port reset failed\n");
      continue;
    }
    usb_device_discovered(&hc->hcd, port, speed);
    hc->debug_stage = "device discovery returned";
    if (port < 32)
      hc->connected_ports |= 1U << port;
  }
}

static void xhci_remove_device(struct usb_hcd *hcd, struct usb_device *dev) {
  struct xhci_controller *hc = hcd->priv;
  struct xhci_slot *slot = dev ? dev->hcd_data : NULL;
  if (!slot || !slot->enabled)
    return;
  for (uint32_t i = 0; i < XHCI_MAX_INTERRUPT_PIPES; i++) {
    struct xhci_interrupt_state *state = &hc->interrupt_pipes[i];
    if (state->pipe.dev != dev)
      continue;
    state->pipe.active = false;
    state->completed = false;
    state->pipe.dev = NULL;
  }
  xhci_submit_command(hc, 0, 0,
                      XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) |
                          ((uint32_t)slot->id << 24),
                      NULL);
  ((uint64_t *)hc->dcbaa)[slot->id] = 0;
  slot->enabled = false;
}

static void xhci_service_ports(struct xhci_controller *hc) {
  uint32_t pending = hc->pending_ports;
  hc->pending_ports = 0;
  for (uint8_t port = 0; port < hc->max_ports; port++) {
    uint32_t offset = XHCI_PORTSC + port * 0x10U;
    uint32_t ps = mmio_read32(hc->op, offset);
    bool changed = (ps & XHCI_PORT_CHANGE_MASK) != 0;
    if (port < 32 && (pending & (1U << port)))
      changed = true;
    if (!changed)
      continue;
    mmio_write32(hc->op, offset,
                 xhci_port_neutral(ps) | (ps & XHCI_PORT_CHANGE_MASK));
    bool connected = (ps & XHCI_PORT_CCS) != 0;
    bool known = port < 32 && (hc->connected_ports & (1U << port));
    if (!connected && known) {
      usb_device_removed(&hc->hcd, port);
      hc->connected_ports &= ~(1U << port);
      klog_puts("[XHCI] Port disconnected\n");
      continue;
    }
    if (!connected || known)
      continue;
    enum usb_speed speed = xhci_usb_speed(XHCI_PORT_SPEED(ps));
    if (speed == USB_SPEED_UNKNOWN || !xhci_reset_port(hc, port)) {
      klog_puts("[XHCI] Port reconnect reset failed\n");
      continue;
    }
    usb_device_discovered(&hc->hcd, port, speed);
    if (port < 32)
      hc->connected_ports |= 1U << port;
    klog_puts("[XHCI] Port reconnected\n");
  }
}

static uint8_t xhci_interval_value(enum usb_speed speed, uint8_t interval) {
  if (!interval)
    return 0;
  if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER ||
      speed == USB_SPEED_SUPER_PLUS)
    return interval > 16 ? 15 : (uint8_t)(interval - 1);
  uint32_t microframes = (uint32_t)interval * 8U;
  uint8_t exponent = 0;
  while ((1U << exponent) < microframes && exponent < 15)
    exponent++;
  return exponent;
}

static int xhci_interrupt_submit(struct xhci_controller *hc,
                                 struct xhci_interrupt_state *state) {
  uint16_t index = state->enqueue;
  struct xhci_trb *trb = &state->ring[index];
  trb->parameter = state->pipe.buffer_phys;
  trb->status = state->pipe.buffer_len;
  trb->control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC |
                 XHCI_TRB_ISP | state->cycle;
  state->expected_trb = state->ring_phys + index * sizeof(*trb);
  state->completed = false;
  state->completion_code = 0;
  state->pipe.actual_length = 0;
  state->enqueue++;
  if (state->enqueue == XHCI_RING_TRBS - 1) {
    state->ring[XHCI_RING_TRBS - 1].control =
        XHCI_TRB_TYPE(XHCI_TRB_LINK) | state->cycle | (1U << 1);
    state->enqueue = 0;
    state->cycle ^= 1;
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);
  struct xhci_slot *slot = state->pipe.dev->hcd_data;
  hc->doorbells[slot->id] = state->endpoint_id;
  return 0;
}

static struct usb_interrupt_pipe *xhci_interrupt_open(
    struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
    uint16_t max_packet, uint8_t interval, void *buffer,
    uint64_t buffer_phys) {
  struct xhci_controller *hc = hcd->priv;
  struct xhci_slot *slot = dev ? dev->hcd_data : NULL;
  if (!slot || !slot->enabled || !(endpoint & 0x80U) || !max_packet ||
      !buffer || !buffer_phys)
    return NULL;
  struct xhci_interrupt_state *state = NULL;
  for (uint32_t i = 0; i < XHCI_MAX_INTERRUPT_PIPES; i++)
    if (!hc->interrupt_pipes[i].pipe.active) {
      state = &hc->interrupt_pipes[i];
      break;
    }
  if (!state)
    return NULL;
  memset(state, 0, sizeof(*state));
  state->endpoint_id = (endpoint & 0x0FU) * 2U + 1U;
  if (state->endpoint_id < 2 || state->endpoint_id > 31 ||
      !xhci_alloc_page(hc, (void **)&state->ring, &state->ring_phys))
    return NULL;
  state->cycle = 1;
  state->ring[XHCI_RING_TRBS - 1].parameter = state->ring_phys;
  state->ring[XHCI_RING_TRBS - 1].control =
      XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_CYCLE | (1U << 1);

  memset(slot->input_context, 0, 4096);
  uint32_t *icc = xhci_context(slot->input_context, 0, hc->context_64);
  uint32_t *sc = xhci_context(slot->input_context, 1, hc->context_64);
  uint32_t *out_sc = xhci_context(slot->output_context, 0, hc->context_64);
  uint32_t context_bytes = hc->context_64 ? 64U : 32U;
  memcpy(sc, out_sc, context_bytes);
  sc[0] = (sc[0] & ~(0x1FU << 27)) |
          ((uint32_t)state->endpoint_id << 27);
  icc[1] = 1U | (1U << state->endpoint_id);
  uint32_t *ep = xhci_context(slot->input_context,
                              state->endpoint_id + 1U, hc->context_64);
  ep[0] = (uint32_t)xhci_interval_value(dev->speed, interval) << 16;
  ep[1] = (3U << 1) | (7U << 3) | ((uint32_t)max_packet << 16);
  uint64_t dequeue = state->ring_phys | state->cycle;
  ep[2] = (uint32_t)dequeue;
  ep[3] = (uint32_t)(dequeue >> 32);
  ep[4] = max_packet | ((uint32_t)max_packet << 16);
  if (!xhci_submit_command(
          hc, slot->input_context_phys, 0,
          XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) |
              ((uint32_t)slot->id << 24),
          NULL)) {
    klog_puts("[XHCI] Configure Endpoint failed\n");
    return NULL;
  }

  uint32_t *out_ep = xhci_context(slot->output_context,
                                  state->endpoint_id, hc->context_64);
  uint32_t ep_state = out_ep[0] & 7U;
  klog_puts("[XHCI] Interrupt endpoint state=");
  klog_uint64(ep_state);
  klog_puts("\n");
  if (ep_state != 1U) {
    klog_puts("[XHCI] Configured interrupt endpoint is not running\n");
    return NULL;
  }

  state->pipe.dev = dev;
  state->pipe.hcd_data = state;
  state->pipe.buffer = buffer;
  state->pipe.buffer_phys = buffer_phys;
  state->pipe.buffer_len = max_packet;
  state->pipe.actual_length = 0;
  state->pipe.endpoint = endpoint;
  state->pipe.active = true;
  if (xhci_interrupt_submit(hc, state) < 0)
    return NULL;
  klog_puts("[XHCI] Interrupt endpoint configured\n");
  return &state->pipe;
}

static bool xhci_interrupt_completed(struct usb_hcd *hcd,
                                     struct usb_interrupt_pipe *pipe) {
  (void)hcd;
  struct xhci_interrupt_state *state = pipe ? pipe->hcd_data : NULL;
  if (!state || !state->completed)
    return false;
  if (state->completion_code == XHCI_COMPLETION_SUCCESS ||
      state->completion_code == 13)
    return true;
  state->completed = false;
  return false;
}

static int xhci_interrupt_resubmit(struct usb_hcd *hcd,
                                   struct usb_interrupt_pipe *pipe) {
  struct xhci_interrupt_state *state = pipe ? pipe->hcd_data : NULL;
  if (!state || !pipe->active)
    return -1;
  return xhci_interrupt_submit(hcd->priv, state);
}

static void xhci_interrupt_cancel(struct usb_hcd *hcd,
                                  struct usb_interrupt_pipe *pipe) {
  struct xhci_controller *hc = hcd->priv;
  struct xhci_interrupt_state *state = pipe ? pipe->hcd_data : NULL;
  struct xhci_slot *slot = pipe && pipe->dev ? pipe->dev->hcd_data : NULL;
  if (!state || !slot)
    return;
  xhci_submit_command(hc, 0, 0,
                      XHCI_TRB_TYPE(XHCI_TRB_STOP_ENDPOINT) |
                          ((uint32_t)state->endpoint_id << 16) |
                          ((uint32_t)slot->id << 24),
                      NULL);
  state->completed = false;
  pipe->active = false;
}

static void xhci_irq(struct registers *regs) {
  for (int i = 0; i < controller_count; i++) {
    struct xhci_controller *hc = &controllers[i];
    if (hc->irq_vector == 0xFF || regs->int_no != hc->irq_vector)
      continue;
    uint32_t iman = mmio_read32(hc->runtime + 0x20, 0);
    hc->interrupts++;
    if (!hc->irq_reported) {
      hc->irq_reported = true;
      klog_puts(hc->msix_enabled ? "[XHCI] MSI-X interrupt received\n"
                                 : "[XHCI] MSI interrupt received\n");
    }
    /* IMAN.IP is RW1C. Acknowledge this interrupter before consuming its
       event ring; ERDP.EHB is cleared as the dequeue pointer advances. */
    mmio_write32(hc->runtime + 0x20, 0,
                 (iman & XHCI_IMAN_IE) | XHCI_IMAN_IP);
    xhci_drain_events(hc, 0);
    xhci_service_ports(hc);
    usb_kbd_poll();
    usb_mouse_poll();
    return;
  }
}

void xhci_msix_watchdog(void) {
  static bool checking;
  if (__atomic_test_and_set(&checking, __ATOMIC_ACQUIRE))
    return;
  for (int i = 0; i < controller_count; i++) {
    struct xhci_controller *hc = &controllers[i];
    if (!hc->running)
      continue;
    /* This reads only cached DMA memory. It recovers lost interrupt edges and
       supports controllers which expose neither MSI-X nor MSI. */
    uint64_t before = hc->events_seen;
    xhci_drain_events(hc, 0);
    if (hc->events_seen != before) {
      xhci_service_ports(hc);
      usb_kbd_poll();
      usb_mouse_poll();
    }
  }
  __atomic_clear(&checking, __ATOMIC_RELEASE);
}

static bool xhci_setup_msix(struct xhci_controller *hc) {
  hc->irq_vector = 0xFF;
  if (!pci_msix_init(hc->pci, &hc->msix))
    return false;
  int vector = interrupt_vector_alloc(xhci_irq);
  if (vector < 0)
    return false;
  hc->irq_vector = (uint8_t)vector;
  if (!pci_msix_program(&hc->msix, 0, hc->irq_vector,
                        (uint8_t)lapic_get_id()) ||
      !pci_msix_enable(&hc->msix)) {
    interrupt_vector_free(hc->irq_vector);
    hc->irq_vector = 0xFF;
    return false;
  }
  /* Unmasking can synchronously deliver an already-pending event. Publish
     the enabled state first so the ISR accepts that first edge. */
  hc->msix_enabled = true;
  pci_msix_mask(&hc->msix, 0, false);
  return true;
}

static bool xhci_setup_msi(struct xhci_controller *hc) {
  int vector = interrupt_vector_alloc(xhci_irq);
  if (vector < 0)
    return false;
  hc->irq_vector = (uint8_t)vector;
  if (!pci_msi_enable(hc->pci, &hc->msi, hc->irq_vector,
                      (uint8_t)lapic_get_id())) {
    interrupt_vector_free(hc->irq_vector);
    hc->irq_vector = 0xFF;
    return false;
  }
  hc->msi_enabled = true;
  return true;
}

static bool xhci_probe(struct pci_device *pci) {
  last_probe_failure = "controller limit";
  if (controller_count >= XHCI_MAX_CONTROLLERS)
    return false;
  struct xhci_controller *hc = &controllers[controller_count];
  memset(hc, 0, sizeof(*hc));
  hc->debug_stage = "probe begin";
  hc->pci = pci;
  hc->mmio_phys = xhci_bar0(pci);
  if (!hc->mmio_phys) {
    last_probe_failure = "invalid BAR0";
    return false;
  }
  // PCI MMIO is not guaranteed to be covered by Limine's HHDM mappings.
  // Map the capability page first; DBOFF/RTSOFF then tell us how much more
  // of the aperture is required.
  if (!xhci_map_mmio(hc->mmio_phys, 4096)) {
    last_probe_failure = "capability MMIO map";
    klog_puts("[XHCI] Failed to map capability MMIO page\n");
    return false;
  }
  hc->cap = (volatile uint8_t *)(hc->mmio_phys + pmm_get_hhdm_offset());
  hc->cap_length = *(volatile uint8_t *)hc->cap;
  hc->version = *(volatile uint16_t *)(hc->cap + 2);
  uint32_t hcs1 = mmio_read32(hc->cap, 0x04);
  uint32_t hcs2 = mmio_read32(hc->cap, 0x08);
  uint32_t hcc1 = mmio_read32(hc->cap, 0x10);
  hc->max_slots = hcs1 & 0xFFU;
  hc->max_interrupters = (hcs1 >> 8) & 0x7FFU;
  hc->max_ports = (hcs1 >> 24) & 0xFFU;
  /* HCSPARAMS2 stores bits 4:0 of Max Scratchpad Buffers in 31:27 and
     bits 9:5 in 25:21. These fields are easy to accidentally reverse. */
  hc->scratchpad_count = ((hcs2 >> 27) & 0x1FU) |
                         (((hcs2 >> 21) & 0x1FU) << 5);
  hc->ac64 = hcc1 & 1U;
  last_ac64 = hc->ac64;
  hc->context_64 = hcc1 & (1U << 2);
  klog_puts("[XHCI] Found controller: version=0x");
  klog_hex32(hc->version);
  klog_puts(" slots=");
  klog_uint64(hc->max_slots);
  klog_puts(" ports=");
  klog_uint64(hc->max_ports);
  klog_puts(" scratchpads=");
  klog_uint64(hc->scratchpad_count);
  klog_puts("\n");
  if (hc->cap_length < 0x20 || !hc->max_slots || !hc->max_ports ||
      !hc->max_interrupters || !(mmio_read32(hc->cap + hc->cap_length,
                                             XHCI_PAGESIZE) & 1U)) {
    last_probe_failure = "capability validation";
    klog_puts("[XHCI] Capability validation failed\n");
    return false;
  }
  uint32_t dboff = mmio_read32(hc->cap, 0x14) & ~3U;
  uint32_t rtsoff = mmio_read32(hc->cap, 0x18) & ~0x1FU;
  uint64_t required = (uint64_t)hc->cap_length + 0x400U +
                      (uint64_t)hc->max_ports * 0x10U;
  uint64_t doorbell_end = (uint64_t)dboff +
                          ((uint64_t)hc->max_slots + 1U) * 4U;
  uint64_t runtime_end = (uint64_t)rtsoff + 0x40U;
  if (doorbell_end > required)
    required = doorbell_end;
  if (runtime_end > required)
    required = runtime_end;
  if (!dboff || !rtsoff || required > 0x100000U ||
      !xhci_map_mmio(hc->mmio_phys, required)) {
    last_probe_failure = "register MMIO extent";
    klog_puts("[XHCI] Invalid or unmappable MMIO register extent\n");
    return false;
  }
  hc->op = hc->cap + hc->cap_length;
  hc->doorbells = (volatile uint32_t *)(hc->cap + dboff);
  hc->runtime = hc->cap + rtsoff;
  hc->debug_stage = "registers mapped";

  uint16_t command = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
  pci_config_write16(pci->bus, pci->slot, pci->func, 0x04,
                     command | 0x0006U | (1U << 10));
  xhci_legacy_handoff(hc, hcc1);
  hc->debug_stage = "legacy handoff complete";
  if (!xhci_halt_reset(hc)) {
    last_probe_failure = "halt/reset timeout";
    klog_puts("[XHCI] Controller halt/reset failed\n");
    return false;
  }
  if (!xhci_alloc_runtime(hc)) {
    last_dma_layer = dma_get_last_failure();
    last_dma_flags = dma_get_last_flags();
    last_dma_phys = dma_get_last_phys();
    last_probe_failure = "runtime DMA allocation";
    klog_puts("[XHCI] Runtime DMA allocation failed\n");
    return false;
  }
  if (!xhci_start(hc)) {
    last_probe_failure = "controller start timeout";
    klog_puts("[XHCI] Controller start failed\n");
    return false;
  }
  hc->debug_stage = "controller running";
  controller_count++;
  if (!xhci_setup_msix(hc))
    xhci_setup_msi(hc);
  if (!xhci_noop_command(hc))
  {
    last_probe_failure = "command ring timeout";
    return false;
  }
  hc->hcd.priv = hc;
  hc->hcd.name = "xhci";
  hc->hcd.control_transfer = NULL;
  hc->hcd.control_device = xhci_control_device;
  hc->hcd.device_prepare = xhci_prepare_device;
  hc->hcd.address_device = xhci_address_device;
  hc->hcd.device_removed = xhci_remove_device;
  hc->hcd.interrupt_open = xhci_interrupt_open;
  hc->hcd.interrupt_completed = xhci_interrupt_completed;
  hc->hcd.interrupt_resubmit = xhci_interrupt_resubmit;
  hc->hcd.interrupt_cancel = xhci_interrupt_cancel;
  klog_puts("[XHCI] Command/event rings operational (v");
  klog_hex32(hc->version);
  if (hc->msix_enabled)
    klog_puts(", MSI-X)\n");
  else if (hc->msi_enabled)
    klog_puts(", MSI)\n");
  else
    klog_puts(", timer fallback)\n");
  xhci_enumerate_ports(hc);
  hc->debug_usbcmd = mmio_read32(hc->op, XHCI_USBCMD);
  hc->debug_usbsts = mmio_read32(hc->op, XHCI_USBSTS);
  last_probe_failure = "none";
  return true;
}

void xhci_init(void) {
  controller_count = 0;
  matched_count = 0;
  last_probe_failure = "no xHCI PCI function";
  last_dma_object = "none";
  last_ac64 = false;
  last_dma_layer = "none";
  last_dma_flags = 0;
  last_dma_phys = 0;
  uint32_t count = pci_get_device_count();
  for (uint32_t i = 0; i < count; i++) {
    struct pci_device *pci = pci_get_device(i);
    if (pci && pci->class_code == 0x0C && pci->subclass == 0x03 &&
        pci->prog_if == 0x30) {
      matched_count++;
      xhci_probe(pci);
    }
  }
  if (!controller_count)
    klog_puts("[XHCI] No operational xHCI controller found\n");
}

int xhci_get_matched_count(void) { return matched_count; }

const char *xhci_get_last_probe_failure(void) { return last_probe_failure; }

const char *xhci_get_last_dma_object(void) { return last_dma_object; }

bool xhci_get_last_ac64(void) { return last_ac64; }

const char *xhci_get_last_dma_layer(void) { return last_dma_layer; }

uint32_t xhci_get_last_dma_flags(void) { return last_dma_flags; }

uint64_t xhci_get_last_dma_phys(void) { return last_dma_phys; }

int xhci_get_controller_count(void) { return controller_count; }

struct xhci_controller *xhci_get_controller(int index) {
  if (index < 0 || index >= controller_count)
    return NULL;
  return &controllers[index];
}

bool xhci_phase2_stress(uint32_t reset_cycles, uint32_t command_count) {
  if (!controller_count)
    return false;
  for (int c = 0; c < controller_count; c++) {
    struct xhci_controller *hc = &controllers[c];
    for (uint32_t i = 0; i < command_count; i++)
      if (!xhci_noop_command(hc))
        return false;
    for (uint32_t i = 0; i < reset_cycles; i++) {
      if (hc->msix_enabled)
        pci_msix_mask(&hc->msix, 0, true);
      if (!xhci_halt_reset(hc))
        return false;
      memset(hc->command_ring, 0, 4096);
      memset(hc->event_ring, 0, 4096);
      hc->command_enqueue = 0;
      hc->event_dequeue = 0;
      hc->command_cycle = 1;
      hc->event_cycle = 1;
      hc->last_command_trb = 0;
      hc->command_ring[XHCI_RING_TRBS - 1].parameter = hc->command_ring_phys;
      hc->command_ring[XHCI_RING_TRBS - 1].control =
          XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_CYCLE | (1U << 1);
      if (!xhci_start(hc))
        return false;
      if (hc->msix_enabled)
        pci_msix_mask(&hc->msix, 0, false);
      if (!xhci_noop_command(hc))
        return false;
    }
  }
  return true;
}
