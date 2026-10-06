#ifndef FIFO_H
#define FIFO_H

#include "vfs.h"
#include <stdint.h>

/**
 * Open a named pipe (FIFO) node with POSIX semantics.
 * Blocks on open() until the peer connects (reader waits for writer,
 * writer waits for reader unless O_NONBLOCK is specified).
 *
 * @param node     Underlying filesystem node (FS_PIPE)
 * @param flags    Open flags (O_RDONLY, O_WRONLY, O_NONBLOCK, etc.)
 * @param out_node Returns a newly allocated per-open descriptor node
 * @return 0 on success, negative error code on failure (-ENOMEM, -ENXIO, -EINTR)
 */
int fifo_open(vfs_node_t *node, uint64_t flags, vfs_node_t **out_node);

/**
 * Destroy the FIFO context associated with a deleted filesystem node.
 */
void fifo_destroy(vfs_node_t *node);

#endif /* FIFO_H */
