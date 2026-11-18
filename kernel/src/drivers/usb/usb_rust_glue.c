/* C shim for the Rust USB class drivers (see usb_rust_glue.h).
 * Empty unless built with USE_RUST_USB=1. */
#ifdef USE_RUST_USB

#include "usb_rust_glue.h"
#include "usb.h"
#include "../../apic/lapic_timer.h"
#include "../../console/klog.h"
#include "../../lib/string.h"
#include "../../mm/dma_alloc.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../sched/sched.h"

#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_DESC_CONFIGURATION 0x02

int usb_rust_control_transfer(void *handle, uint8_t request_type,
                              uint8_t request, uint16_t value, uint16_t index,
                              void *data, uint16_t len) {
  struct usb_control_request req = {
      .request_type = request_type,
      .request = request,
      .value = value,
      .index = index,
      .length = len,
  };
  /* The core returns 0 on success; Rust expects a byte count.  The HCD does
   * not report the actual length, so report the requested one (descriptor
   * parsers bound themselves by bLength). */
  int rc = usb_control_transfer((struct usb_device *)handle, &req, data, len);
  return rc < 0 ? rc : (int)len;
}

size_t usb_rust_device_descriptor(void *handle, uint8_t *out, size_t cap) {
  struct usb_device *dev = (struct usb_device *)handle;
  size_t n = sizeof(struct usb_device_descriptor);
  if (!dev || !out)
    return 0;
  if (n > cap)
    n = cap;
  memcpy(out, &dev->desc, n);
  return n;
}

int usb_rust_get_config_descriptor(void *handle, uint8_t index, uint8_t *out,
                                   uint16_t cap) {
  uint8_t head[9];
  uint16_t total;
  int rc;

  if (!handle || !out)
    return -1;
  rc = usb_rust_control_transfer(handle, 0x80, USB_REQ_GET_DESCRIPTOR,
                                 (uint16_t)((USB_DESC_CONFIGURATION << 8) | index),
                                 0, head, sizeof(head));
  if (rc < 0)
    return -1;
  total = (uint16_t)(head[2] | ((uint16_t)head[3] << 8));
  if (total < sizeof(head))
    return -1;
  if (total > cap)
    return -2;
  rc = usb_rust_control_transfer(handle, 0x80, USB_REQ_GET_DESCRIPTOR,
                                 (uint16_t)((USB_DESC_CONFIGURATION << 8) | index),
                                 0, out, total);
  return rc < 0 ? -1 : (int)total;
}

uint64_t usb_rust_dma_phys(void *buf) {
  /* HHDM translation is only valid for single-page, physically contiguous
   * kmalloc memory; Phase 4 switches to a page allocator for DMA. */
  return (uint64_t)buf - pmm_get_hhdm_offset();
}

void *usb_rust_interrupt_open(void *handle, uint8_t endpoint,
                              uint16_t max_packet, uint8_t interval,
                              void *buf) {
  if (!handle || !buf)
    return NULL;
  return usb_interrupt_open((struct usb_device *)handle, endpoint, max_packet,
                            interval, buf, usb_rust_dma_phys(buf));
}

bool usb_rust_interrupt_completed(void *pipe) {
  return pipe && usb_interrupt_completed((struct usb_interrupt_pipe *)pipe);
}

int usb_rust_interrupt_resubmit(void *pipe) {
  if (!pipe)
    return -1;
  return usb_interrupt_resubmit((struct usb_interrupt_pipe *)pipe);
}

void usb_rust_interrupt_cancel(void *pipe) {
  if (pipe)
    usb_interrupt_cancel((struct usb_interrupt_pipe *)pipe);
}

int usb_rust_hub_configure(void *handle, uint8_t nports, bool multi_tt,
                           uint8_t tt_think_time) {
  return usb_hub_configure((struct usb_device *)handle, nports, multi_tt,
                           tt_think_time);
}

int usb_rust_device_speed(void *handle) {
  return handle ? (int)((struct usb_device *)handle)->speed : 0;
}

uint8_t usb_rust_device_depth(void *handle) {
  return handle ? ((struct usb_device *)handle)->depth : 0;
}

int usb_rust_hub_child_discovered(void *handle, uint8_t port, int speed) {
  if (!handle)
    return -1;
  return usb_hub_child_discovered((struct usb_device *)handle, port,
                                  (enum usb_speed)speed);
}

void usb_rust_hub_child_removed(void *handle, uint8_t port) {
  if (handle)
    usb_hub_child_removed((struct usb_device *)handle, port);
}

void *usb_rust_bulk_open(void *handle, uint8_t endpoint, uint16_t max_packet,
                         uint8_t max_burst) {
  if (!handle)
    return NULL;
  return usb_bulk_open((struct usb_device *)handle, endpoint, max_packet,
                       max_burst);
}

int usb_rust_bulk_transfer(void *pipe, void *buf, uint64_t phys, uint32_t len,
                           uint32_t *actual, uint32_t timeout_ms) {
  (void)buf;
  if (!pipe)
    return USB_BULK_GONE;
  return usb_bulk_transfer((struct usb_bulk_pipe *)pipe, buf, phys, len,
                           actual, timeout_ms);
}

int usb_rust_bulk_clear_halt(void *pipe) {
  if (!pipe)
    return USB_BULK_GONE;
  return usb_clear_halt((struct usb_bulk_pipe *)pipe);
}

void usb_rust_bulk_close(void *pipe) {
  if (pipe)
    usb_bulk_close((struct usb_bulk_pipe *)pipe);
}

void *usb_rust_dma_alloc(size_t size, uint64_t *phys_out, void **handle_out) {
  dma_buffer_t *buf = dma_alloc(size, DMA_FLAG_32BIT);
  if (!buf)
    return NULL;
  if (phys_out)
    *phys_out = buf->phys;
  if (handle_out)
    *handle_out = buf;
  return buf->virt;
}

void usb_rust_dma_free(void *handle) {
  if (handle)
    dma_free((dma_buffer_t *)handle);
}

void *usb_rust_kmalloc(size_t size) { return kmalloc(size); }

void usb_rust_kfree(void *ptr) {
  if (ptr)
    kfree(ptr);
}

/* Yield-based mutex: the owner flag is an atomic test-and-set; contenders
 * sched_yield() until it clears.  Safe to hold across sleeping transfers. */
struct usb_rust_mutex {
  int locked;
};

void *usb_rust_mutex_create(void) {
  struct usb_rust_mutex *m = kmalloc(sizeof(*m));
  if (!m)
    return NULL;
  m->locked = 0;
  return m;
}

void usb_rust_mutex_lock(void *mutex) {
  struct usb_rust_mutex *m = (struct usb_rust_mutex *)mutex;
  if (!m)
    return;
  while (__atomic_test_and_set(&m->locked, __ATOMIC_ACQUIRE))
    sched_yield();
}

void usb_rust_mutex_unlock(void *mutex) {
  struct usb_rust_mutex *m = (struct usb_rust_mutex *)mutex;
  if (m)
    __atomic_clear(&m->locked, __ATOMIC_RELEASE);
}

void usb_rust_mutex_destroy(void *mutex) {
  if (mutex)
    kfree(mutex);
}

int usb_rust_thread_spawn(void (*entry)(void)) {
  if (!entry)
    return -1;
  /* USB enumeration (HID probing, descriptor parsing) runs on this thread;
   * the default 16 KB stack overflowed into adjacent pages. */
  return sched_create_kernel_thread_sized(entry, NULL, true, 64 * 1024) ? 0
                                                                       : -1;
}

void usb_rust_msleep(uint32_t ms) {
  if (ms == 0)
    return;
  struct thread *self = sched_get_current();
  if (self && !self->is_idle && lapic_timer_get_ticks() > 0) {
    self->wakeup_ticks = lapic_timer_get_ms() + ms;
    self->state = THREAD_SLEEPING;
    sched_yield();
    self->wakeup_ticks = 0;
    if (self->state == THREAD_SLEEPING)
      self->state = THREAD_RUNNING;
    return;
  }
  lapic_timer_sleep(ms);
}

uint64_t usb_rust_now_ms(void) { return lapic_timer_get_ms(); }

void usb_rust_klog(const char *s, size_t len) {
  if (!s)
    return;
  for (size_t i = 0; i < len; i++)
    klog_putchar(s[i]);
}

void usb_rust_diag_append(void *diag, const char *s, size_t len) {
  if (!diag || !s || len == 0)
    return;
  usb_diag_printf((struct usb_diag *)diag, "%.*s", (int)len, s);
}

#endif /* USE_RUST_USB */
