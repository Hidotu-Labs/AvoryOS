#ifndef DRIVERS_USB_USB_RUST_GLUE_H
#define DRIVERS_USB_USB_RUST_GLUE_H

/* Narrow C shim between the USB core and the Rust class drivers.
 *
 * Rules: opaque handles (void *), plain integers, and byte buffers only.
 * Nothing here exposes struct usb_device or struct block_device layout to
 * Rust.  Compiled to real code only with USE_RUST_USB=1.
 *
 * Return conventions: functions returning int give 0 or a positive value on
 * success and a negative value on failure. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Rust-side entry point, called by usb.c after VID/PID is logged. */
void rust_usb_device_probe(void *handle, uint16_t vid, uint16_t pid);

/* Hub class driver (drivers/usb/hub).  rust_usb_hub_init starts the worker
 * thread once, from usb_init.  rust_usb_hub_probe records a hub and never
 * blocks, so it is safe from IRQ and timer context.  rust_usb_hub_removed
 * flags the hub, cancels its interrupt pipe and never blocks; the worker
 * frees the hub later.  rust_usb_diag appends hub state to a usb_diag. */
void rust_usb_hub_init(void);
void rust_usb_hub_probe(void *handle);
void rust_usb_hub_removed(void *handle);
void rust_usb_diag(void *diag);

/* --- USB ----------------------------------------------------------------- */

/* Control transfer on a device handle (struct usb_device *). */
int usb_rust_control_transfer(void *handle, uint8_t request_type,
                              uint8_t request, uint16_t value, uint16_t index,
                              void *data, uint16_t len);

/* Copy the cached device descriptor (18 bytes).  Returns bytes copied. */
size_t usb_rust_device_descriptor(void *handle, uint8_t *out, size_t cap);

/* Fetch configuration descriptor `index` in full.  Returns the number of
 * bytes written, -1 on transfer failure, or -2 if cap is smaller than
 * wTotalLength (the caller retries with a larger buffer). */
int usb_rust_get_config_descriptor(void *handle, uint8_t index, uint8_t *out,
                                   uint16_t cap);

/* Interrupt pipes.  `buf` must come from usb_rust_kmalloc and be no larger
 * than one page; its physical address is derived from the HHDM mapping. */
void *usb_rust_interrupt_open(void *handle, uint8_t endpoint,
                              uint16_t max_packet, uint8_t interval,
                              void *buf);
bool usb_rust_interrupt_completed(void *pipe);
int usb_rust_interrupt_resubmit(void *pipe);
void usb_rust_interrupt_cancel(void *pipe);

/* Hub parameters for a hub device (Slot Context Hub/MTT/ports/think time).
 * Returns 0 on success, -1 on failure. */
int usb_rust_hub_configure(void *handle, uint8_t nports, bool multi_tt,
                           uint8_t tt_think_time);

/* Device facts for the hub driver.  Speed is an enum usb_speed value. */
int usb_rust_device_speed(void *handle);
uint8_t usb_rust_device_depth(void *handle);

/* Attach or detach the device behind a hub's 1-based port.  Attach returns 0
 * on success, -1 when the topology refuses it. */
int usb_rust_hub_child_discovered(void *handle, uint8_t port, int speed);
void usb_rust_hub_child_removed(void *handle, uint8_t port);

/* Bulk pipes.  One pipe per endpoint address; transfers on a pipe are
 * serialised by the caller.  `buf` must be DMA memory from
 * usb_rust_dma_alloc() and `phys` its physical address.  Transfers block
 * until completion, stall, timeout or device removal.  Return codes match
 * usb.h: 0 ok (short IN packet is 0 with *actual < len), USB_BULK_STALL (-2),
 * USB_BULK_TIMEOUT (-3), USB_BULK_GONE (-4), other failures -1. */
void *usb_rust_bulk_open(void *handle, uint8_t endpoint, uint16_t max_packet,
                         uint8_t max_burst);
int usb_rust_bulk_transfer(void *pipe, void *buf, uint64_t phys, uint32_t len,
                           uint32_t *actual, uint32_t timeout_ms);
int usb_rust_bulk_clear_halt(void *pipe);
void usb_rust_bulk_close(void *pipe);

/* Physically contiguous DMA memory (below 4 GiB).  Returns the virtual
 * address, or NULL.  *phys_out receives the physical address and *handle_out
 * the handle that usb_rust_dma_free() takes. */
void *usb_rust_dma_alloc(size_t size, uint64_t *phys_out, void **handle_out);
void usb_rust_dma_free(void *handle);

/* --- Kernel primitives --------------------------------------------------- */

void *usb_rust_kmalloc(size_t size);
void usb_rust_kfree(void *ptr);

/* Mutex that yields the CPU while contended, so it may be held across
 * sleeping transfers.  Not FIFO and not IRQ-safe: task context only. */
void *usb_rust_mutex_create(void);
void usb_rust_mutex_lock(void *mutex);
void usb_rust_mutex_unlock(void *mutex);
void usb_rust_mutex_destroy(void *mutex);

/* Physical address of a kmalloc buffer.  Valid only for single-page,
 * physically contiguous kmalloc memory (HHDM translation).  Phase 4 will use
 * a page allocator for DMA. */
uint64_t usb_rust_dma_phys(void *buf);

/* Spawn a kernel thread running entry().  Returns 0 on success. */
int usb_rust_thread_spawn(void (*entry)(void));

void usb_rust_msleep(uint32_t ms);
uint64_t usb_rust_now_ms(void);

/* Write len bytes to the kernel log (not NUL-terminated). */
void usb_rust_klog(const char *s, size_t len);

/* Append len bytes to a struct usb_diag (served at /proc/usb). */
void usb_rust_diag_append(void *diag, const char *s, size_t len);

/* Block bridge for the Rust mass-storage driver.  The C side owns the
 * struct block_device, its sdX name, partitions, /dev and sysfs nodes; Rust
 * only supplies these callbacks.  `ctx` is passed back unchanged.  Each
 * callback returns 0 on success, negative on failure, and is never called
 * after usb_rust_block_unregister() returns.  Counts are in device sectors. */
struct usb_rust_block_ops {
  int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
  int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
  int (*flush)(void *ctx);
};

/* Register a removable disk for `usb_dev` and scan its partitions.  Returns
 * an opaque block handle, or NULL on failure. */
void *usb_rust_block_register(void *usb_dev, uint32_t sector_size,
                              uint64_t total_sectors, bool read_only,
                              void *ctx, const struct usb_rust_block_ops *ops);

/* Remove the disk and its partitions.  In-flight and later I/O on it fails
 * with an error instead of reaching the callbacks. */
void usb_rust_block_unregister(void *block);

#endif
