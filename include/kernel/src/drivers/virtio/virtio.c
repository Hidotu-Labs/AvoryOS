#include "drivers/virtio/virtio.h"
#include "console/klog.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include <stdint.h>

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

static inline uint64_t virtq_desc_size(uint16_t num) {
  return (uint64_t)num * sizeof(struct virtq_desc);
}

static inline uint64_t virtq_avail_size(uint16_t num) {
  return sizeof(uint16_t) * 2 + sizeof(uint16_t) * num + sizeof(uint16_t);
}

static inline uint64_t virtq_used_size(uint16_t num) {
  return sizeof(uint16_t) * 2 + sizeof(struct virtq_used_elem) * num +
         sizeof(uint16_t);
}

static inline void virtq_dma_wmb(void) {
  __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void virtq_dma_rmb(void) {
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static inline void virtq_dma_mb(void) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static bool virtq_size_valid(uint16_t num) {
  return num >= 2 && num <= VIRTQ_MAX_SIZE && (num & (num - 1)) == 0;
}

bool virtq_init(struct virtqueue *vq, uint16_t num) {
  if (!vq || !virtq_size_valid(num))
    return false;

  memset(vq, 0, sizeof(*vq));
  spinlock_init(&vq->lock);

  uint64_t desc_bytes = ALIGN_UP(virtq_desc_size(num), 16);
  uint64_t avail_bytes = ALIGN_UP(virtq_avail_size(num), 4);
  uint64_t used_bytes = ALIGN_UP(virtq_used_size(num), 4);
  uint64_t total = desc_bytes + avail_bytes + used_bytes;
  uint32_t pages_needed = (uint32_t)((total + PAGE_SIZE - 1) / PAGE_SIZE);

  void *phys = pmm_alloc_pages(pages_needed);
  if (!phys) {
    klog_puts("[VIRTIO] Failed to allocate virtqueue\n");
    return false;
  }

  uint64_t phys_base = (uint64_t)phys;
  uint64_t virt_base = phys_base + pmm_get_hhdm_offset();
  memset((void *)virt_base, 0, (size_t)pages_needed * PAGE_SIZE);

  vq->desc_phys = phys_base;
  vq->avail_phys = phys_base + desc_bytes;
  vq->used_phys = phys_base + desc_bytes + avail_bytes;
  vq->alloc_phys = phys_base;
  vq->alloc_pages = pages_needed;
  vq->desc = (volatile struct virtq_desc *)virt_base;
  vq->avail = (volatile struct virtq_avail *)(virt_base + desc_bytes);
  vq->used = (volatile struct virtq_used *)(virt_base + desc_bytes + avail_bytes);
  vq->num = num;
  vq->num_free = num;
  vq->free_head = 0;

  for (uint16_t i = 0; i < num; i++) {
    vq->desc[i].next = (uint16_t)(i + 1);
    vq->desc[i].flags = 0;
  }
  vq->desc[num - 1].next = 0;
  return true;
}

void virtq_destroy(struct virtqueue *vq) {
  if (!vq || !vq->alloc_phys)
    return;
  pmm_free_pages((void *)vq->alloc_phys, vq->alloc_pages);
  memset(vq, 0, sizeof(*vq));
}

static int virtq_alloc_desc_locked(struct virtqueue *vq) {
  if (!vq->num_free)
    return -1;

  uint16_t idx = vq->free_head;
  if (idx >= vq->num || vq->desc_in_use[idx])
    return -1;

  vq->free_head = vq->desc[idx].next;
  vq->num_free--;
  vq->desc_in_use[idx] = true;
  return idx;
}

static bool virtq_free_chain_locked(struct virtqueue *vq, uint16_t head) {
  if (head >= vq->num || !vq->chain_head[head] || !vq->desc_in_use[head])
    return false;

  uint16_t idx = head;
  for (uint16_t walked = 0; walked < vq->num; walked++) {
    if (idx >= vq->num || !vq->desc_in_use[idx])
      return false;

    bool has_next = (vq->desc[idx].flags & VIRTQ_DESC_F_NEXT) != 0;
    uint16_t next = vq->desc[idx].next;
    vq->desc[idx].addr = 0;
    vq->desc[idx].len = 0;
    vq->desc[idx].flags = 0;
    vq->desc[idx].next = vq->free_head;
    vq->desc_in_use[idx] = false;
    vq->free_head = idx;
    vq->num_free++;

    if (!has_next) {
      vq->chain_head[head] = false;
      vq->cookies[head] = NULL;
      return true;
    }
    idx = next;
  }
  return false;
}

int virtq_add_buf_readonly(struct virtqueue *vq, uint64_t phys_addr,
                           uint32_t len) {
  if (!vq || !len)
    return -1;
  spinlock_acquire(&vq->lock);
  int idx = virtq_alloc_desc_locked(vq);
  if (idx >= 0) {
    vq->desc[idx].addr = phys_addr;
    vq->desc[idx].len = len;
    vq->desc[idx].flags = 0;
    vq->desc[idx].next = 0;
    vq->chain_head[idx] = true;
  }
  spinlock_release(&vq->lock);
  return idx;
}

int virtq_add_buf_chain(struct virtqueue *vq, uint64_t req_phys,
                        uint32_t req_len, uint64_t resp_phys,
                        uint32_t resp_len) {
  if (!vq || !req_len || !resp_len)
    return -1;

  spinlock_acquire(&vq->lock);
  if (vq->num_free < 2) {
    spinlock_release(&vq->lock);
    return -1;
  }

  int head = virtq_alloc_desc_locked(vq);
  int tail = virtq_alloc_desc_locked(vq);
  vq->desc[head].addr = req_phys;
  vq->desc[head].len = req_len;
  vq->desc[head].flags = VIRTQ_DESC_F_NEXT;
  vq->desc[head].next = (uint16_t)tail;
  vq->desc[tail].addr = resp_phys;
  vq->desc[tail].len = resp_len;
  vq->desc[tail].flags = VIRTQ_DESC_F_WRITE;
  vq->desc[tail].next = 0;
  vq->chain_head[head] = true;
  spinlock_release(&vq->lock);
  return head;
}

void virtq_kick(struct virtqueue *vq) {
  if (!vq || !vq->notify_addr)
    return;
  virtq_dma_mb();
  *vq->notify_addr = vq->queue_index;
}

bool virtq_poll(struct virtqueue *vq, uint32_t *id, uint32_t *len) {
  if (!vq || !id || !len)
    return false;
  spinlock_acquire(&vq->lock);
  virtq_dma_rmb();
  if (vq->last_used_idx == vq->used->idx) {
    spinlock_release(&vq->lock);
    return false;
  }
  uint16_t slot = (uint16_t)(vq->last_used_idx % vq->num);
  *id = vq->used->ring[slot].id;
  *len = vq->used->ring[slot].len;
  vq->last_used_idx++;
  spinlock_release(&vq->lock);
  return true;
}

void virtq_free_desc(struct virtqueue *vq, uint16_t head) {
  if (!vq)
    return;
  spinlock_acquire(&vq->lock);
  if (!virtq_free_chain_locked(vq, head))
    vq->stats.rejected++;
  spinlock_release(&vq->lock);
}

static int virtq_submit_internal(struct virtqueue *vq,
                 const struct virtq_iov *out, size_t out_count,
                 const struct virtq_iov *in, size_t in_count,
                 void *cookie, bool notify) {
  if (!vq || (out_count && !out) || (in_count && !in))
    return -1;
  size_t count = out_count + in_count;
  if (!count || count > vq->num)
    return -1;

  spinlock_acquire(&vq->lock);
  if (vq->num_free < count) {
    vq->stats.rejected++;
    spinlock_release(&vq->lock);
    return -1;
  }

  uint16_t ids[VIRTQ_MAX_SIZE];
  for (size_t i = 0; i < count; i++)
    ids[i] = (uint16_t)virtq_alloc_desc_locked(vq);

  for (size_t i = 0; i < count; i++) {
    bool writable = i >= out_count;
    const struct virtq_iov *iov = writable ? &in[i - out_count] : &out[i];
    vq->desc[ids[i]].addr = iov->phys_addr;
    vq->desc[ids[i]].len = iov->len;
    vq->desc[ids[i]].flags = writable ? VIRTQ_DESC_F_WRITE : 0;
    if (i + 1 < count) {
      vq->desc[ids[i]].flags |= VIRTQ_DESC_F_NEXT;
      vq->desc[ids[i]].next = ids[i + 1];
    } else {
      vq->desc[ids[i]].next = 0;
    }
  }

  uint16_t head = ids[0];
  vq->chain_head[head] = true;
  vq->cookies[head] = cookie;
  uint16_t avail_idx = vq->avail->idx;
  vq->avail->ring[avail_idx % vq->num] = head;
  virtq_dma_wmb();
  vq->avail->idx = (uint16_t)(avail_idx + 1);
  vq->stats.submitted++;
  virtq_dma_mb();
  if (notify && vq->notify_addr)
    *vq->notify_addr = vq->queue_index;
  spinlock_release(&vq->lock);
  return head;
}

int virtq_submit(struct virtqueue *vq, const struct virtq_iov *out,
                 size_t out_count, const struct virtq_iov *in,
                 size_t in_count, void *cookie) {
  return virtq_submit_internal(vq, out, out_count, in, in_count, cookie, true);
}

int virtq_submit_deferred(struct virtqueue *vq, const struct virtq_iov *out,
                          size_t out_count, const struct virtq_iov *in,
                          size_t in_count, void *cookie) {
  return virtq_submit_internal(vq, out, out_count, in, in_count, cookie, false);
}

bool virtq_poll_complete(struct virtqueue *vq, void **cookie, uint32_t *len) {
  if (!vq || !cookie || !len)
    return false;

  spinlock_acquire(&vq->lock);
  virtq_dma_rmb();
  if (vq->last_used_idx == vq->used->idx) {
    spinlock_release(&vq->lock);
    return false;
  }

  uint16_t slot = (uint16_t)(vq->last_used_idx % vq->num);
  uint32_t id = vq->used->ring[slot].id;
  uint32_t used_len = vq->used->ring[slot].len;
  vq->last_used_idx++;

  if (id >= vq->num || !vq->chain_head[id] || !vq->desc_in_use[id]) {
    vq->stats.invalid_used++;
    spinlock_release(&vq->lock);
    return false;
  }

  *cookie = vq->cookies[id];
  *len = used_len;
  if (!virtq_free_chain_locked(vq, (uint16_t)id)) {
    vq->stats.invalid_used++;
    spinlock_release(&vq->lock);
    return false;
  }
  vq->stats.completed++;
  spinlock_release(&vq->lock);
  return true;
}

bool virtq_is_idle(struct virtqueue *vq) {
  if (!vq)
    return true;
  spinlock_acquire(&vq->lock);
  bool idle = vq->num_free == vq->num &&
              vq->last_used_idx == vq->used->idx;
  spinlock_release(&vq->lock);
  return idle;
}

void virtq_get_stats(struct virtqueue *vq, struct virtq_stats *out) {
  if (!vq || !out)
    return;
  spinlock_acquire(&vq->lock);
  *out = vq->stats;
  spinlock_release(&vq->lock);
}

static void virtq_test_complete(struct virtqueue *vq, uint16_t head,
                                uint32_t len) {
  uint16_t used_idx = vq->used->idx;
  vq->used->ring[used_idx % vq->num].id = head;
  vq->used->ring[used_idx % vq->num].len = len;
  virtq_dma_wmb();
  vq->used->idx = (uint16_t)(used_idx + 1);
}

bool virtio_self_test(void) {
  klog_puts("[VIRTIO] Phase 1 split-queue stress test starting\n");
  size_t free_before = pmm_get_free_pages();
  struct virtqueue vq;
  if (!virtq_init(&vq, 64)) {
    klog_puts("[VIRTIO] FAIL: queue allocation\n");
    return false;
  }

  struct virtq_iov out = { .phys_addr = 0x1000, .len = 64 };
  struct virtq_iov in = { .phys_addr = 0x2000, .len = 128 };
  for (uint32_t i = 0; i < 1000000; i++) {
    void *expected = (void *)(uintptr_t)(i + 1);
    int head = virtq_submit(&vq, &out, 1, &in, 1, expected);
    if (head < 0) {
      klog_puts("[VIRTIO] FAIL: stress submission\n");
      virtq_destroy(&vq);
      return false;
    }
    virtq_test_complete(&vq, (uint16_t)head, in.len);
    void *cookie = NULL;
    uint32_t len = 0;
    if (!virtq_poll_complete(&vq, &cookie, &len) ||
        cookie != expected || len != in.len) {
      klog_puts("[VIRTIO] FAIL: stress completion\n");
      virtq_destroy(&vq);
      return false;
    }
  }

  if (!virtq_is_idle(&vq) || vq.avail->idx != (uint16_t)1000000 ||
      vq.used->idx != (uint16_t)1000000) {
    klog_puts("[VIRTIO] FAIL: index wrap or idle state\n");
    virtq_destroy(&vq);
    return false;
  }

  int heads[32];
  for (int i = 0; i < 32; i++) {
    heads[i] = virtq_submit(&vq, &out, 1, &in, 1,
                            (void *)(uintptr_t)(i + 1));
    if (heads[i] < 0) {
      klog_puts("[VIRTIO] FAIL: exhaustion setup\n");
      virtq_destroy(&vq);
      return false;
    }
  }
  if (virtq_submit(&vq, &out, 1, &in, 1, NULL) >= 0) {
    klog_puts("[VIRTIO] FAIL: exhaustion not detected\n");
    virtq_destroy(&vq);
    return false;
  }
  for (int i = 0; i < 32; i++) {
    virtq_test_complete(&vq, (uint16_t)heads[i], in.len);
    void *cookie;
    uint32_t len;
    if (!virtq_poll_complete(&vq, &cookie, &len)) {
      klog_puts("[VIRTIO] FAIL: exhaustion recovery\n");
      virtq_destroy(&vq);
      return false;
    }
  }

  uint16_t bad_slot = vq.used->idx;
  vq.used->ring[bad_slot % vq.num].id = vq.num;
  vq.used->ring[bad_slot % vq.num].len = 0;
  vq.used->idx = (uint16_t)(bad_slot + 1);
  void *cookie;
  uint32_t len;
  if (virtq_poll_complete(&vq, &cookie, &len)) {
    klog_puts("[VIRTIO] FAIL: invalid used id accepted\n");
    virtq_destroy(&vq);
    return false;
  }

  struct virtq_stats stats;
  virtq_get_stats(&vq, &stats);
  if (stats.submitted != 1000032 || stats.completed != 1000032 ||
      stats.rejected != 1 || stats.invalid_used != 1 ||
      vq.num_free != vq.num) {
    klog_puts("[VIRTIO] FAIL: counter mismatch\n");
    virtq_destroy(&vq);
    return false;
  }

  virtq_destroy(&vq);
  if (pmm_get_free_pages() != free_before) {
    klog_puts("[VIRTIO] FAIL: queue page leak\n");
    return false;
  }

  klog_puts("[VIRTIO] PASS: 1000000 cycles, wrap, exhaustion, invalid-id, teardown\n");
  return true;
}
