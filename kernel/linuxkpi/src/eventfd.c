#include <linux/eventfd.h>
#include <linuxkpi/native_eventfd.h>

struct eventfd_ctx *eventfd_ctx_fdget(int fd)
{
    void *node;
    int ret = asc_eventfd_get(fd, &node);

    return ret ? ERR_PTR(ret) : (struct eventfd_ctx *)node;
}

void eventfd_ctx_put(struct eventfd_ctx *ctx)
{
    asc_eventfd_put(ctx);
}

void eventfd_signal_mask(struct eventfd_ctx *ctx, __poll_t mask)
{
    asc_eventfd_signal(ctx, (unsigned int)mask);
}
