/* Block bridge between the Rust mass-storage driver and the block layer.
 * See usb_rust_glue.h for the contract.
 *
 * A disk is a struct block_device named sdX (major 8) whose read/write/flush
 * entry points forward to the Rust ops table.  Removal sets `gone` first, so
 * new I/O fails without touching the driver, then waits for calls already
 * inside the driver to leave.  Only after that does unregister return.  The
 * structs are freed by the block layer once the last reference (open /dev
 * node, partition, mount) is dropped. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/uaccess.h"
#include "drivers/storage/block.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "usb_rust_glue.h"

struct usb_rust_disk {
  void *ctx;
  const struct usb_rust_block_ops *ops;
  bool read_only;
  /* Calls currently inside ops->*.  Incremented before the gone check so
   * unregister can wait for them to drain. */
  uint32_t inflight;
};

static struct usb_rust_disk *disk_of(struct block_device *dev) {
  return (struct usb_rust_disk *)dev->driver_data;
}

/* Enter/leave a driver call.  Returns false when the disk is going away; the
 * caller must then not touch ctx or ops. */
static bool usb_rust_disk_enter(struct usb_rust_disk *d,
                                struct block_device *dev) {
  __atomic_add_fetch(&d->inflight, 1, __ATOMIC_SEQ_CST);
  if (block_device_gone(dev)) {
    __atomic_sub_fetch(&d->inflight, 1, __ATOMIC_SEQ_CST);
    return false;
  }
  return true;
}

static void usb_rust_disk_leave(struct usb_rust_disk *d) {
  __atomic_sub_fetch(&d->inflight, 1, __ATOMIC_SEQ_CST);
}

static bool usb_rust_range_ok(const struct block_device *dev, uint64_t lba,
                              uint32_t count) {
  return count != 0 && lba < dev->total_sectors &&
         (uint64_t)count <= dev->total_sectors - lba;
}

static int usb_rust_block_read(struct block_device *dev, uint64_t lba,
                               uint32_t count, void *buf) {
  struct usb_rust_disk *d = disk_of(dev);
  if (!d || !buf || !usb_rust_range_ok(dev, lba, count))
    return -1;
  uint32_t ss = dev->sector_size ? dev->sector_size : 512;
  uint64_t bytes = (uint64_t)count * ss;
  if (bytes > 0xFFFFFFFFULL)
    return -1;

  void *kbuf = buf;
  bool bounce = is_user_ptr((uint64_t)buf);
  if (bounce) {
    kbuf = kmalloc((size_t)bytes);
    if (!kbuf)
      return -1;
  }

  if (!usb_rust_disk_enter(d, dev)) {
    if (bounce)
      kfree(kbuf);
    return -1;
  }
  int ret = d->ops->read(d->ctx, lba, count, kbuf);
  usb_rust_disk_leave(d);
  if (ret == 0 && bounce && copy_to_user(buf, kbuf, (size_t)bytes) != 0)
    ret = -1;
  if (bounce)
    kfree(kbuf);
  if (ret == 0)
    block_stat_read(dev, count);
  return ret;
}

static int usb_rust_block_write(struct block_device *dev, uint64_t lba,
                                uint32_t count, const void *buf) {
  struct usb_rust_disk *d = disk_of(dev);
  if (!d || d->read_only || !buf || !usb_rust_range_ok(dev, lba, count))
    return -1;
  uint32_t ss = dev->sector_size ? dev->sector_size : 512;
  uint64_t bytes = (uint64_t)count * ss;
  if (bytes > 0xFFFFFFFFULL)
    return -1;

  const void *kbuf = buf;
  void *alloc = NULL;
  if (is_user_ptr((uint64_t)buf)) {
    alloc = kmalloc((size_t)bytes);
    if (!alloc)
      return -1;
    if (copy_from_user(alloc, buf, (size_t)bytes) != 0) {
      kfree(alloc);
      return -1;
    }
    kbuf = alloc;
  }

  if (!usb_rust_disk_enter(d, dev)) {
    kfree(alloc);
    return -1;
  }
  int ret = d->ops->write(d->ctx, lba, count, kbuf);
  usb_rust_disk_leave(d);
  kfree(alloc);
  if (ret == 0)
    block_stat_write(dev, count);
  return ret;
}

static int usb_rust_block_flush(struct block_device *dev) {
  struct usb_rust_disk *d = disk_of(dev);
  if (!d || !d->ops->flush)
    return -1;
  if (!usb_rust_disk_enter(d, dev))
    return -1;
  int ret = d->ops->flush(d->ctx);
  usb_rust_disk_leave(d);
  return ret;
}

/* Disk index to its Linux name: 0 -> sda, 25 -> sdz, 26 -> sdaa, 701 -> sdzz,
 * 702 -> sdaaa.  The suffix is bijective base 26 (no zero digit). */
static void usb_rust_disk_name(uint32_t index, char name[16]) {
  char suffix[8];
  int len = 0;
  uint32_t n = index + 1;
  while (n > 0 && len < (int)sizeof(suffix)) {
    n--;
    suffix[len++] = (char)('a' + n % 26);
    n /= 26;
  }
  name[0] = 's';
  name[1] = 'd';
  for (int i = 0; i < len; i++)
    name[2 + i] = suffix[len - 1 - i];
  name[2 + len] = '\0';
}

/* First sdX name not in use.  Registration is single-threaded per driver, so
 * no reservation is needed between the check and block_register().  At most
 * BLOCK_MAX_DEVICES names can be taken, so one of the first
 * BLOCK_MAX_DEVICES + 1 is free whenever the registry has room. */
static bool usb_rust_pick_name(char name[16]) {
  for (uint32_t i = 0; i <= BLOCK_MAX_DEVICES; i++) {
    usb_rust_disk_name(i, name);
    if (!block_name_in_use(name))
      return true;
  }
  return false;
}

/* Last reference gone: no I/O can reach the driver any more. */
static void usb_rust_disk_release(struct block_device *dev) {
  kfree(dev->driver_data);
  kfree(dev);
}

void *usb_rust_block_register(void *usb_dev, uint32_t sector_size,
                              uint64_t total_sectors, bool read_only,
                              void *ctx, const struct usb_rust_block_ops *ops) {
  (void)usb_dev;
  if (!ops || !ops->read || sector_size == 0 || total_sectors == 0)
    return NULL;

  struct usb_rust_disk *d = kmalloc(sizeof(*d));
  struct block_device *dev = kmalloc(sizeof(*dev));
  if (!d || !dev) {
    kfree(d);
    kfree(dev);
    return NULL;
  }
  memset(d, 0, sizeof(*d));
  memset(dev, 0, sizeof(*dev));

  if (!usb_rust_pick_name(dev->name)) {
    kfree(d);
    kfree(dev);
    return NULL;
  }

  d->ctx = ctx;
  d->ops = ops;
  d->read_only = read_only;

  dev->sector_size = sector_size;
  dev->total_sectors = total_sectors;
  dev->read_only = read_only;
  dev->removable = true;
  dev->read_sectors = usb_rust_block_read;
  if (!read_only && ops->write)
    dev->write_sectors = usb_rust_block_write;
  if (ops->flush)
    dev->flush = usb_rust_block_flush;
  dev->driver_data = d;
  dev->release = usb_rust_disk_release;

  /* block_register() scans partitions itself once the disk is published, so
   * there is no separate block_scan_partitions() call here: a second scan
   * would add every partition twice. */
  if (block_register(dev) != 0) {
    kfree(d);
    kfree(dev);
    return NULL;
  }
  return dev;
}

/* Blocks until no driver call is in flight.  Must run in task context: a
 * stuck transfer on the calling thread would prevent it from finishing. */
void usb_rust_block_unregister(void *block) {
  struct block_device *dev = (struct block_device *)block;
  if (!dev || block_is_partition(dev))
    return;

  /* Hold our own reference across the drain: block_unregister() drops the
   * registry's, which may be the last one. */
  block_ref(dev);
  __atomic_store_n(&dev->gone, true, __ATOMIC_SEQ_CST);
  block_unregister(dev);

  /* Partitions were removed with the disk, so this drains their calls too.
   * Stale partitions, /dev vnodes and mounts keep the struct alive through
   * their references and only ever see gone. */
  struct usb_rust_disk *d = disk_of(dev);
  if (d)
    while (__atomic_load_n(&d->inflight, __ATOMIC_SEQ_CST) != 0)
      sched_yield();
  block_put(dev);
}
