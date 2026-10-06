#include "vfs.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include <stdint.h>

#define XATTR_MAX_VALUE_SIZE 65536
#define XATTR_MAX_NAME_LEN 255

#define XATTR_CREATE 0x1
#define XATTR_REPLACE 0x2

#define XATTR_E2BIG 7
#define XATTR_ENOMEM 12
#define XATTR_EEXIST 17
#define XATTR_EINVAL 22
#define XATTR_ERANGE 34
#define XATTR_ENODATA 61
#define XATTR_EOPNOTSUPP 95

typedef struct xattr_entry {
  char *name;
  uint8_t *value;
  uint32_t size;
  struct xattr_entry *next;
} xattr_entry_t;

static spinlock_t xattr_lock = SPINLOCK_INIT;

/* Only user.* is stored. security/trusted/system need LSM and privilege
 * checks this kernel doesn't implement; report EOPNOTSUPP there exactly as
 * before so no caller mistakes a stub for enforcement. */
static bool xattr_namespace_ok(const char *name) {
  return strncmp(name, "user.", 5) == 0;
}

static xattr_entry_t *xattr_find_locked(xattr_entry_t *head,
                                        const char *name) {
  for (xattr_entry_t *e = head; e; e = e->next) {
    if (strcmp(e->name, name) == 0)
      return e;
  }
  return NULL;
}

int vfs_setxattr(vfs_node_t *node, const char *name, const void *value,
                 uint32_t size, int flags) {
  if (!node || !name || (!value && size != 0))
    return -XATTR_EINVAL;
  size_t name_len = strlen(name);
  if (name_len == 0 || name_len > XATTR_MAX_NAME_LEN)
    return -XATTR_EINVAL;
  if (!xattr_namespace_ok(name))
    return -XATTR_EOPNOTSUPP;
  if (size > XATTR_MAX_VALUE_SIZE)
    return -XATTR_E2BIG;
  if ((flags & ~(XATTR_CREATE | XATTR_REPLACE)) != 0)
    return -XATTR_EINVAL;

  char *name_copy = kmalloc(name_len + 1);
  if (!name_copy)
    return -XATTR_ENOMEM;
  memcpy(name_copy, name, name_len + 1);

  uint8_t *value_copy = NULL;
  if (size > 0) {
    value_copy = kmalloc(size);
    if (!value_copy) {
      kfree(name_copy);
      return -XATTR_ENOMEM;
    }
    memcpy(value_copy, value, size);
  }

  spinlock_acquire(&xattr_lock);
  xattr_entry_t *head = (xattr_entry_t *)node->xattrs;
  xattr_entry_t *existing = xattr_find_locked(head, name);
  if ((flags & XATTR_CREATE) && existing) {
    spinlock_release(&xattr_lock);
    kfree(name_copy);
    if (value_copy)
      kfree(value_copy);
    return -XATTR_EEXIST;
  }
  if ((flags & XATTR_REPLACE) && !existing) {
    spinlock_release(&xattr_lock);
    kfree(name_copy);
    if (value_copy)
      kfree(value_copy);
    return -XATTR_ENODATA;
  }
  if (existing) {
    if (existing->value)
      kfree(existing->value);
    existing->value = value_copy;
    existing->size = size;
    spinlock_release(&xattr_lock);
    kfree(name_copy);
    return 0;
  }
  xattr_entry_t *entry = kmalloc(sizeof(xattr_entry_t));
  if (!entry) {
    spinlock_release(&xattr_lock);
    kfree(name_copy);
    if (value_copy)
      kfree(value_copy);
    return -XATTR_ENOMEM;
  }
  entry->name = name_copy;
  entry->value = value_copy;
  entry->size = size;
  entry->next = head;
  node->xattrs = entry;
  spinlock_release(&xattr_lock);
  return 0;
}

int vfs_getxattr(vfs_node_t *node, const char *name, void *value,
                 uint32_t size) {
  if (!node || !name)
    return -XATTR_EINVAL;
  if (!xattr_namespace_ok(name))
    return -XATTR_EOPNOTSUPP;

  spinlock_acquire(&xattr_lock);
  xattr_entry_t *e =
      xattr_find_locked((xattr_entry_t *)node->xattrs, name);
  if (!e) {
    spinlock_release(&xattr_lock);
    return -XATTR_ENODATA;
  }
  uint32_t needed = e->size;
  if (!value || size == 0) {
    spinlock_release(&xattr_lock);
    return (int)needed;
  }
  if (size < needed) {
    spinlock_release(&xattr_lock);
    return -XATTR_ERANGE;
  }
  if (needed > 0)
    memcpy(value, e->value, needed);
  spinlock_release(&xattr_lock);
  return (int)needed;
}

int vfs_listxattr(vfs_node_t *node, char *list, uint32_t size) {
  if (!node)
    return -XATTR_EINVAL;

  spinlock_acquire(&xattr_lock);
  uint32_t total = 0;
  for (xattr_entry_t *e = (xattr_entry_t *)node->xattrs; e; e = e->next)
    total += (uint32_t)strlen(e->name) + 1;
  if (!list || size == 0) {
    spinlock_release(&xattr_lock);
    return (int)total;
  }
  if (size < total) {
    spinlock_release(&xattr_lock);
    return -XATTR_ERANGE;
  }
  uint32_t pos = 0;
  for (xattr_entry_t *e = (xattr_entry_t *)node->xattrs; e; e = e->next) {
    uint32_t len = (uint32_t)strlen(e->name) + 1;
    memcpy(list + pos, e->name, len);
    pos += len;
  }
  spinlock_release(&xattr_lock);
  return (int)total;
}

int vfs_removexattr(vfs_node_t *node, const char *name) {
  if (!node || !name)
    return -XATTR_EINVAL;
  if (!xattr_namespace_ok(name))
    return -XATTR_EOPNOTSUPP;

  spinlock_acquire(&xattr_lock);
  xattr_entry_t **link = (xattr_entry_t **)&node->xattrs;
  while (*link && strcmp((*link)->name, name) != 0)
    link = &(*link)->next;
  if (!*link) {
    spinlock_release(&xattr_lock);
    return -XATTR_ENODATA;
  }
  xattr_entry_t *dead = *link;
  *link = dead->next;
  spinlock_release(&xattr_lock);

  kfree(dead->name);
  if (dead->value)
    kfree(dead->value);
  kfree(dead);
  return 0;
}

void vfs_xattr_clear(vfs_node_t *node) {
  if (!node)
    return;
  if (__atomic_load_n(&node->xattrs, __ATOMIC_ACQUIRE) == NULL)
    return;

  spinlock_acquire(&xattr_lock);
  xattr_entry_t *head = (xattr_entry_t *)node->xattrs;
  node->xattrs = NULL;
  spinlock_release(&xattr_lock);

  while (head) {
    xattr_entry_t *next = head->next;
    kfree(head->name);
    if (head->value)
      kfree(head->value);
    kfree(head);
    head = next;
  }
}
