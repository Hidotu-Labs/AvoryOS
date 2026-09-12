/* SPDX-License-Identifier: MIT */
#ifndef AVORYOS_UAPI_KPI_DMABUF_H
#define AVORYOS_UAPI_KPI_DMABUF_H

/* Test ABI for /dev/kpi_dmabuf (Phase 2 LinuxKPI exit-criteria device).
 *
 * The device is a development aid, not a stable userspace interface: it
 * exposes Linux dma-buf objects to a small self-test binary so the dma-buf
 * fd, mmap and sync_file paths can be exercised end to end from userspace.
 *
 * Shared verbatim by kernel/linuxkpi/src/kpi_dmabuf_testdev.c and
 * userland/test_kpi_dmabuf.c.
 */

#include <linux/ioctl.h>
#include <linux/types.h>

#define KPI_DMABUF_MAGIC 'K'

/* Allocate a BO of `arg` 4 KiB pages, export it as a dma-buf and return the
 * new fd (the state keeps the buffer alive independently of that fd). */
#define KPI_DMABUF_IOC_ALLOC _IOWR(KPI_DMABUF_MAGIC, 0x01, unsigned long)
/* Return the number of vm_ops->close invocations on test BO mappings. */
#define KPI_DMABUF_IOC_CLOSE_COUNT _IOR(KPI_DMABUF_MAGIC, 0x02, unsigned long)
/* Import the dma-buf in descriptor `arg` (dma_buf_get) and drop it again. */
#define KPI_DMABUF_IOC_IMPORT _IOW(KPI_DMABUF_MAGIC, 0x03, int)
/* Create an unsignaled test fence, wrap it in a sync_file and return its fd. */
#define KPI_DMABUF_IOC_SYNC_FD _IOR(KPI_DMABUF_MAGIC, 0x04, int)
/* Signal the test fence created by KPI_DMABUF_IOC_SYNC_FD. */
#define KPI_DMABUF_IOC_SIGNAL _IO(KPI_DMABUF_MAGIC, 0x05)
/* sync_file_get_fence(fd = arg) + dma_fence_wait_timeout(10 s). */
#define KPI_DMABUF_IOC_WAIT _IOW(KPI_DMABUF_MAGIC, 0x06, int)
/* Native PMM free-page count including per-CPU caches (soak/leak checks). */
#define KPI_DMABUF_IOC_PMM_FREE _IOR(KPI_DMABUF_MAGIC, 0x07, unsigned long)
/* Page count of the open file's current BO (0 when none). */
#define KPI_DMABUF_IOC_BO_PAGES _IOR(KPI_DMABUF_MAGIC, 0x08, unsigned long)
/* Check every page of the current BO: head page, refcount == 1, managed. */
#define KPI_DMABUF_IOC_VERIFY _IO(KPI_DMABUF_MAGIC, 0x09)

#define KPI_DMABUF_MAX_PAGES 4096 /* 16 MiB per BO */

#endif /* AVORYOS_UAPI_KPI_DMABUF_H */
