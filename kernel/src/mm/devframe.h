#ifndef MM_DEVFRAME_H
#define MM_DEVFRAME_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Device-frame lifetime. Host-visible blob memory is not PMM-managed, so no
 * per-PTE page reference keeps it alive. Instead every user leaf that maps a
 * frame inside a registered device range holds one reference on the owner.
 * The owner is released by a worker thread once the last reference is gone,
 * so teardown never runs in the middle of a page-table walk.
 *
 * Install/remove hooks are called from the page-table code under vmm_lock.
 * They never sleep and never take a lock that vmm_lock is ordered before.
 */
struct devframe_ops {
  void (*get)(void *owner);
  /* true when the last reference was dropped */
  bool (*put)(void *owner);
  /* worker context: may sleep; the owner must not be used afterwards */
  void (*release)(void *owner);
};

/* Register [base, base + size) as owned by `owner`. Ranges must not overlap. */
int devframe_register(uint64_t base, uint64_t size, void *owner,
                      const struct devframe_ops *ops);
/* Drop the range entry. Called from the owner's release path. */
void devframe_unregister(void *owner);
/* Run fn(owner) later on the worker thread, where it may sleep and flush the
 * TLB. Safe from any context that is not under vmm_lock. False if not queued. */
bool devframe_defer(void *owner, void (*fn)(void *owner));

/* Core hooks: a user leaf for `phys` was installed / removed at `va`.
 * Frames that are PMM-managed or outside any registered range are ignored. */
void devframe_pte_install(uint64_t va, uint64_t phys);
void devframe_pte_remove(uint64_t va, uint64_t phys);

#endif
