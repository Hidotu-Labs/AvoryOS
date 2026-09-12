#ifndef __AVORY_LINUXKPI_MMAP_LOCK_H
#define __AVORY_LINUXKPI_MMAP_LOCK_H

/* AvoryOS overlay for <linux/mmap_lock.h>.
 *
 * Upstream's mmap_lock is an rwsem embedded in struct mm_struct and taken
 * around VMA walks and page faults.  AvoryOS keeps VMA/mapping state under its
 * own native locks, and no imported code walks a Linux mm (struct mm_struct
 * has no VMA tree here), so every entry point is an inert inline.  The
 * upstream call sites stay syntactically correct and balanced; TTM's fault
 * path only needs mmap_read_unlock() around dma_resv waits.
 *
 * When a future phase grows a real Linux mm, this file must map to the native
 * address-space lock instead.  Update docs/linuxkpi-gaps.md with that change.
 */

#include <linux/types.h>

struct mm_struct;

static inline void mmap_init_lock(struct mm_struct *mm) { (void)mm; }

static inline void mmap_write_lock(struct mm_struct *mm) { (void)mm; }
static inline void mmap_write_lock_nested(struct mm_struct *mm, int subclass) {
  (void)mm;
  (void)subclass;
}
static inline int mmap_write_lock_killable(struct mm_struct *mm) {
  (void)mm;
  return 0;
}
static inline void mmap_write_unlock(struct mm_struct *mm) { (void)mm; }
static inline void mmap_write_downgrade(struct mm_struct *mm) { (void)mm; }

static inline void mmap_read_lock(struct mm_struct *mm) { (void)mm; }
static inline int mmap_read_lock_killable(struct mm_struct *mm) {
  (void)mm;
  return 0;
}
static inline bool mmap_read_trylock(struct mm_struct *mm) {
  (void)mm;
  return true;
}
static inline void mmap_read_unlock(struct mm_struct *mm) { (void)mm; }
static inline void mmap_read_unlock_non_owner(struct mm_struct *mm) {
  (void)mm;
}

static inline void mmap_assert_locked(struct mm_struct *mm) { (void)mm; }
static inline void mmap_assert_write_locked(struct mm_struct *mm) { (void)mm; }
static inline int mmap_lock_is_contended(struct mm_struct *mm) {
  (void)mm;
  return 0;
}

static inline void vma_end_write_all(struct mm_struct *mm) { (void)mm; }

#endif /* __AVORY_LINUXKPI_MMAP_LOCK_H */
