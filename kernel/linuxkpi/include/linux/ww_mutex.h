#ifndef __AVORY_LINUXKPI_WW_MUTEX_H
#define __AVORY_LINUXKPI_WW_MUTEX_H

/* Minimal Linux <linux/ww_mutex.h> overlay.
 *
 * ww_mutex is the wound/wait mutex used by DRM modeset and dma_resv to avoid
 * ABBA deadlocks.  This implementation is a plain sleeping mutex with the
 * acquire-context stamps recorded; wounding is NOT implemented, so a
 * multi-lock acquire sequence is deadlock-free only when every caller
 * acquires in one global order - the invariant TTM/amdgpu keep when
 * reserving a BO set.  A runtime WARN cannot distinguish that safe ordered
 * contention from a real cycle without wait tracking, so the ordering
 * assumption is documented (P6 C5 gap log) and exercised by the ordered
 * multi-lock stress in test_phase1_core6.c instead.
 * Implementation: linuxkpi/src/ww_mutex.c. */

#include <linux/mutex.h>
#include <linux/sched.h>

struct ww_class {
  unsigned long stamp;
};

struct ww_acquire_ctx {
  struct task_struct *task;
  unsigned long stamp;
  unsigned int acquired;
  bool wounded;
};

struct ww_mutex {
  struct mutex base;
  struct ww_class *ww_class;
  struct ww_acquire_ctx *ctx;
};

#define DEFINE_WW_CLASS(name) struct ww_class name = {0}
#define DEFINE_WW_MUTEX(name, class)                                          \
  struct ww_mutex name = {                                                    \
    .base = __MUTEX_INITIALIZER(name.base),                                   \
    .ww_class = (class), .ctx = NULL,                                         \
  }

void ww_mutex_init(struct ww_mutex *lock, struct ww_class *ww_class);

/* A ww_class with no lockdep state. */
#define DEFINE_WD_CLASS(name) struct ww_class name = {0}

void ww_mutex_destroy(struct ww_mutex *lock);

void ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class);
void ww_acquire_done(struct ww_acquire_ctx *ctx);
void ww_acquire_fini(struct ww_acquire_ctx *ctx);

int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
int ww_mutex_lock_interruptible(struct ww_mutex *lock,
                                struct ww_acquire_ctx *ctx);
int ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
int ww_mutex_lock_slow_interruptible(struct ww_mutex *lock,
                                     struct ww_acquire_ctx *ctx);
int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
void ww_mutex_unlock(struct ww_mutex *lock);
bool ww_mutex_is_locked(struct ww_mutex *lock);

#endif /* __AVORY_LINUXKPI_WW_MUTEX_H */
