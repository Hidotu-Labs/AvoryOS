#ifndef __AVORY_LINUXKPI_WW_MUTEX_H
#define __AVORY_LINUXKPI_WW_MUTEX_H

/* Native sleeping transaction locks for DRM modeset and dma_resv.
 * Ownership, context publication and waiter ordering share base.wait_lock.
 * DEFINE_WW_CLASS selects wound-wait; DEFINE_WD_CLASS selects wait-die.
 * Priority inheritance and lockdep remain unsupported. */

#include <linux/mutex.h>
#include <linux/sched.h>

struct ww_class {
  unsigned long stamp;
  bool is_wait_die;
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

#define DEFINE_WW_CLASS(name) struct ww_class name = {0, false}
#define DEFINE_WW_MUTEX(name, class)                                          \
  struct ww_mutex name = {                                                    \
    .base = __MUTEX_INITIALIZER(name.base),                                   \
    .ww_class = (class), .ctx = NULL,                                         \
  }

void ww_mutex_init(struct ww_mutex *lock, struct ww_class *ww_class);

#define DEFINE_WD_CLASS(name) struct ww_class name = {0, true}

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
