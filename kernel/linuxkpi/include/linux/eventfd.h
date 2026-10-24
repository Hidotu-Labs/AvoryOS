#ifndef AVORY_LINUXKPI_EVENTFD_H
#define AVORY_LINUXKPI_EVENTFD_H

/* Native eventfds share their counter and poll queue with userspace. The
 * imported !CONFIG_EVENTFD stubs cannot be used by DRM syncobj callbacks. */
#include <linux/types.h>
#include <linux/err.h>
#include <uapi/linux/eventfd.h>

struct eventfd_ctx;
struct eventfd_ctx *eventfd_ctx_fdget(int fd);
void eventfd_ctx_put(struct eventfd_ctx *ctx);
void eventfd_signal_mask(struct eventfd_ctx *ctx, __poll_t mask);

static inline void eventfd_signal(struct eventfd_ctx *ctx)
{
    eventfd_signal_mask(ctx, 0);
}

static inline bool eventfd_signal_allowed(void)
{
    return true;
}

#endif
