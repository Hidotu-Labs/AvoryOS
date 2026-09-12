/* LinuxKPI wound/wait mutex (PI-less, no wounding yet).
 * See linux/ww_mutex.h for the contract and deviations. */

#include <linux/errno.h>
#include <linux/ww_mutex.h>

void ww_mutex_init(struct ww_mutex *lock, struct ww_class *ww_class) {
  mutex_init(&lock->base);
  lock->ww_class = ww_class;
  lock->ctx = NULL;
}

void ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class) {
  ctx->task = current;
  ctx->stamp =
      __atomic_add_fetch(&ww_class->stamp, 1, __ATOMIC_ACQ_REL);
  ctx->acquired = 0;
  ctx->wounded = false;
}

void ww_acquire_done(struct ww_acquire_ctx *ctx) { (void)ctx; }

void ww_acquire_fini(struct ww_acquire_ctx *ctx) { (void)ctx; }

/* Re-acquiring a lock this context already holds is not a deadlock: upstream
 * reports -EALREADY and DRM's modeset_lock() treats that as success ("we
 * already hold the lock.. this is fine").  Without this check the plain mutex
 * underneath self-deadlocks (observed with drm_helper_probe_single_connector_
 * modes() re-taking connection_mutex). */
static inline bool ww_mutex_held_by(struct ww_mutex *lock,
                                    struct ww_acquire_ctx *ctx) {
  return ctx && lock->ctx == ctx;
}

int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) {
  if (ww_mutex_held_by(lock, ctx))
    return -EALREADY;

  mutex_lock(&lock->base);
  if (ctx) {
    lock->ctx = ctx;
    ctx->acquired++;
  }
  return 0;
}

int ww_mutex_lock_interruptible(struct ww_mutex *lock,
                                struct ww_acquire_ctx *ctx) {
  int ret;

  if (ww_mutex_held_by(lock, ctx))
    return -EALREADY;

  ret = mutex_lock_interruptible(&lock->base);
  if (!ret && ctx) {
    lock->ctx = ctx;
    ctx->acquired++;
  }
  return ret;
}

int ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) {
  return ww_mutex_lock(lock, ctx);
}

int ww_mutex_lock_slow_interruptible(struct ww_mutex *lock,
                                     struct ww_acquire_ctx *ctx) {
  return ww_mutex_lock_interruptible(lock, ctx);
}

/* Upstream returns 1 when the lock was acquired and 0 when it is busy
 * (kernel/locking/mutex.c: "Returns 1 if the mutex has been acquired
 * successfully, 0 otherwise").  The `int` prototype and the bool casts in
 * <linux/dma-resv.h> / drm_modeset_lock.c both rely on that truthy
 * convention.  There is deliberately no same-context special case: a
 * re-trylock by the owning context fails like it would upstream. */
int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) {
  if (!mutex_trylock(&lock->base))
    return 0;
  if (ctx) {
    lock->ctx = ctx;
    ctx->acquired++;
  }
  return 1;
}

void ww_mutex_unlock(struct ww_mutex *lock) {
  lock->ctx = NULL;
  mutex_unlock(&lock->base);
}

void ww_mutex_destroy(struct ww_mutex *lock) { mutex_destroy(&lock->base); }

bool ww_mutex_is_locked(struct ww_mutex *lock) {
  return mutex_is_locked(&lock->base);
}
