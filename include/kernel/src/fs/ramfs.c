#include "ramfs.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "arch/uaccess.h"

static uint32_t next_inode = 1;

void ramfs_free_file_data(ramfs_file_t *file) {
  if (!file || !file->data)
    return;

  if (file->data_is_pmm) {
    uint64_t hhdm = pmm_get_hhdm_offset();
    size_t pages = (file->capacity + 4095) / 4096;
    klog_ramfs_free(file->data, file->capacity, true, pages);
    pmm_free_pages((void *)((uint64_t)file->data - hhdm), pages);
  } else {
    klog_ramfs_free(file->data, file->capacity, false, 0);
    kfree(file->data);
  }

  file->data = NULL;
  file->capacity = 0;
  file->data_is_pmm = 0;
}

static int ramfs_chmod(vfs_node_t *node, uint16_t permission);
static int ramfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid);

// VFS Implementations

uint32_t ramfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                    uint8_t *buffer) {
  if (!node || !node->device)
    return 0;

  ramfs_file_t *file = (ramfs_file_t *)node->device;
  if (offset >= node->length)
    return 0;

  if (offset + size > node->length) {
    size = node->length - offset;
  }

  if (is_user_ptr((uint64_t)buffer)) {
    unsigned long uncopied = copy_to_user(buffer, file->data + offset, size);
    if (uncopied > 0) {
      uint32_t copied = size - (uint32_t)uncopied;
      return copied > 0 ? copied : (uint32_t)-14;
    }
  } else {
    memcpy(buffer, file->data + offset, size);
  }
  return size;
}

static uint8_t *ramfs_alloc_buffer(ramfs_file_t *file, uint32_t req_cap, uint32_t *out_cap, uint8_t *out_is_pmm) {
  if (file->data_is_pmm || req_cap >= 4096) {
    uint32_t aligned_cap = (req_cap + 4095) & ~4095U;
    if (aligned_cap < 4096)
      aligned_cap = 4096;
    size_t pages = aligned_cap / 4096;
    void *phys = pmm_alloc_pages(pages);
    if (phys) {
      uint64_t hhdm = pmm_get_hhdm_offset();
      *out_cap = aligned_cap;
      *out_is_pmm = 1;
      return (uint8_t *)((uint64_t)phys + hhdm);
    }
  }

  *out_cap = req_cap;
  *out_is_pmm = 0;
  return (uint8_t *)kmalloc(req_cap);
}

uint32_t ramfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                     uint8_t *buffer) {
  if (!node || !node->device)
    return 0;

  ramfs_file_t *file = (ramfs_file_t *)node->device;

  // Auto-resize buffer if needed
  if (offset + size > file->capacity) {
    uint32_t req_cap = (offset + size) * 2; // Double the required size
    if (req_cap < 512)
      req_cap = 512;

    uint32_t new_cap = 0;
    uint8_t new_is_pmm = 0;
    uint8_t *new_data = ramfs_alloc_buffer(file, req_cap, &new_cap, &new_is_pmm);
    if (!new_data)
      return 0; // OOM

    if (file->data) {
      uint32_t copy_len = node->length < file->capacity ? node->length : file->capacity;
      memcpy(new_data, file->data, copy_len);
      ramfs_free_file_data(file);
    }
    file->data = new_data;
    file->capacity = new_cap;
    file->data_is_pmm = new_is_pmm;
  }

  if (is_user_ptr((uint64_t)buffer)) {
    unsigned long uncopied = copy_from_user(file->data + offset, buffer, size);
    if (uncopied > 0) {
      uint32_t copied = size - (uint32_t)uncopied;
      if (offset + copied > node->length) {
        node->length = offset + copied;
      }
      return copied > 0 ? copied : (uint32_t)-14;
    }
  } else {
    memcpy(file->data + offset, buffer, size);
  }
  if (offset + size > node->length) {
    node->length = offset + size;
  }

  return size;
}

int ramfs_truncate(vfs_node_t *node, uint32_t new_len) {
  if (!node || node->flags != FS_FILE || !node->device)
    return -1;

  ramfs_file_t *file = (ramfs_file_t *)node->device;
  if (new_len == 0) {
    if (file->data) {
      ramfs_free_file_data(file);
    }
    node->length = 0;
    return 0;
  }

  if (new_len > file->capacity) {
    uint32_t new_cap = 0;
    uint8_t new_is_pmm = 0;
    uint8_t *new_data = ramfs_alloc_buffer(file, new_len, &new_cap, &new_is_pmm);
    if (!new_data)
      return -1; // ENOMEM
    if (file->data) {
      uint32_t copy_len = node->length < file->capacity ? node->length : file->capacity;
      memcpy(new_data, file->data, copy_len);
      ramfs_free_file_data(file);
    }
    if (new_cap > node->length)
      memset(new_data + node->length, 0, new_cap - node->length);
    file->data = new_data;
    file->capacity = new_cap;
    file->data_is_pmm = new_is_pmm;
  } else if (new_len > node->length) {
    memset(file->data + node->length, 0, new_len - node->length);
  }

  node->length = new_len;
  return 0;
}

int ramfs_fallocate(vfs_node_t *node, int mode, uint32_t offset, uint32_t len) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE || !node->device)
    return -1;

  ramfs_file_t *file = (ramfs_file_t *)node->device;
  uint64_t needed64 = (uint64_t)offset + (uint64_t)len;
  if (needed64 > 0xFFFFFFFFULL)
    return -1;
  uint32_t needed = (uint32_t)needed64;

  klog_debugf("[RAMFS] fallocate node=%s mode=%llu offset=%llu len=%llu old_len=%llu old_cap=%llu old_pmm=%llu needed=%llu\n",
              node->name, (unsigned long long)mode, (unsigned long long)offset,
              (unsigned long long)len, (unsigned long long)node->length,
              (unsigned long long)file->capacity, (unsigned long long)file->data_is_pmm,
              (unsigned long long)needed);

  if (needed > file->capacity) {
    uint32_t new_cap = 0;
    uint8_t new_is_pmm = 0;
    uint8_t *new_data = ramfs_alloc_buffer(file, needed, &new_cap, &new_is_pmm);
    if (!new_data)
      return -1;
    if (file->data) {
      uint32_t copy_len = node->length < file->capacity ? node->length : file->capacity;
      memcpy(new_data, file->data, copy_len);
      ramfs_free_file_data(file);
    }
    // Zero initialize new capacity range
    if (new_cap > node->length)
      memset(new_data + node->length, 0, new_cap - node->length);
    file->data = new_data;
    file->capacity = new_cap;
    file->data_is_pmm = new_is_pmm;
  }

  // mode 1 is FALLOC_FL_KEEP_SIZE
  if (!(mode & 0x01)) {
    if (needed > node->length) {
      node->length = needed;
    }
  }

  return 0;
}

static struct dirent *ramfs_readdir(vfs_node_t *node, uint32_t index) {
  if (!node || !node->device)
    return 0;
  ramfs_dir_t *dir = (ramfs_dir_t *)node->device;

  static struct dirent d;
  memset(&d, 0, sizeof(struct dirent));

  if (index == 0) {
    strcpy(d.name, ".");
    d.ino = node->inode;
    d.d_type = DT_DIR;
    return &d;
  }
  if (index == 1) {
    strcpy(d.name, "..");
    d.ino = node->inode;
    d.d_type = DT_DIR;
    return &d;
  }

  index -= 2;
  /* Resume from the cursor for sequential getdents; otherwise rescan. */
  child_node_t *cursor = dir->cursor;
  uint32_t cursor_index = dir->cursor_index;
  child_node_t *curr;
  if (cursor && index == cursor_index + 1) {
    curr = cursor->next;
  } else {
    curr = dir->children;
    for (uint32_t i = 0; i < index && curr; i++)
      curr = curr->next;
  }
  dir->cursor = curr;
  dir->cursor_index = index;

  if (curr) {
    strcpy(d.name, curr->node->name);
    d.ino = curr->node->inode;
    d.d_type = vfs_dtype(curr->node->flags);
    return &d;
  }

  return 0; // End of directory
}

static vfs_node_t *ramfs_finddir(vfs_node_t *node, char *name) {
  if (!node || !node->device)
    return 0;

  ramfs_dir_t *dir = (ramfs_dir_t *)node->device;

  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    return node;
  }

  child_node_t *curr = dir->children;
  while (curr) {
    if (strcmp(curr->node->name, name) == 0) {
      return curr->node;
    }
    curr = curr->next;
  }

  return 0;
}

// Helper to construct a new node linked to ramfs standard APIs
static vfs_node_t *ramfs_make_node(char *name, uint16_t perm, uint32_t type) {
  vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
  if (!n)
    return 0;
  vfs_node_init(n);

  strncpy(n->name, name, 127);
  n->mask = perm;
  n->uid = 0;
  n->gid = 0;
  n->flags = type | FS_PERSISTENT;
  n->inode = next_inode++;
  n->length = 0;
  n->impl = 0;
  n->ptr = 0;

  if (type == FS_DIRECTORY) {
    ramfs_dir_t *d = kmalloc(sizeof(ramfs_dir_t));
    d->children = 0;
    d->cursor = 0;
    d->cursor_index = 0;
    n->device = d;
    n->readdir = ramfs_readdir;
    n->finddir = ramfs_finddir;
  } else if (type == FS_FILE) {
    ramfs_file_t *f = kmalloc(sizeof(ramfs_file_t));
    f->data = 0;
    f->capacity = 0;
    f->data_is_pmm = 0;
    n->device = f;
    n->read = ramfs_read;
    n->write = ramfs_write;
    n->truncate = ramfs_truncate;
    n->fallocate = ramfs_fallocate;
  }

  n->chmod = ramfs_chmod;
  n->chown = ramfs_chown;

  return n;
}

// Internal helper to add node to dir
static void ramfs_add_child(vfs_node_t *parent, vfs_node_t *child) {
  ramfs_dir_t *dir = (ramfs_dir_t *)parent->device;
  child_node_t *cn = kmalloc(sizeof(child_node_t));
  cn->node = child;
  cn->next = dir->children;
  dir->children = cn;
}

static int ramfs_create(vfs_node_t *node, char *name, uint16_t permission) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return -1;
  if (ramfs_finddir(node, name) != 0)
    return -1; // File exists

  vfs_node_t *new_node = ramfs_make_node(name, permission, FS_FILE);
  if (!new_node)
    return -1;

  ramfs_add_child(node, new_node);
  return 0;
}

static int ramfs_mknod(vfs_node_t *node, char *name, uint16_t permission,
                       uint32_t flags, void *device) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return -1;
  if (ramfs_finddir(node, name) != 0)
    return -1; // Node exists

  vfs_node_t *new_node = ramfs_make_node(name, permission, flags);
  if (!new_node)
    return -1;

  new_node->device = device;
  ramfs_add_child(node, new_node);
  return 0;
}

static int ramfs_unlink(vfs_node_t *node, char *name);
static int ramfs_rmdir(vfs_node_t *node, char *name);
static int ramfs_rename(vfs_node_t *node, char *old_name, char *new_name);

static int ramfs_mkdir(vfs_node_t *node, char *name, uint16_t permission) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return -1;
  if (ramfs_finddir(node, name) != 0)
    return -1; // Directory exists

  vfs_node_t *new_node = ramfs_make_node(name, permission, FS_DIRECTORY);
  if (!new_node)
    return -1;

  // Assign directory pointers
  new_node->create = ramfs_create;
  new_node->mkdir = ramfs_mkdir;
  new_node->unlink = ramfs_unlink;
  new_node->rmdir = ramfs_rmdir;
  new_node->rename = ramfs_rename;
  new_node->mknod = ramfs_mknod;
  new_node->chmod = ramfs_chmod;
  new_node->chown = ramfs_chown;

  ramfs_add_child(node, new_node);
  return 0;
}

static int ramfs_chmod(vfs_node_t *node, uint16_t permission) {
  if (!node)
    return -1;
  node->mask = permission & 0x0FFF;
  return 0;
}

static int ramfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid) {
  if (!node)
    return -1;
  node->uid = uid;
  node->gid = gid;
  return 0;
}

// ramfs_unlink: Remove a file from a directory
static int ramfs_unlink(vfs_node_t *node, char *name) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY || !node->device)
    return -1;

  ramfs_dir_t *dir = (ramfs_dir_t *)node->device;
  child_node_t *prev = 0;
  child_node_t *curr = dir->children;

  while (curr) {
    if (strcmp(curr->node->name, name) == 0) {
      // Don't allow unlinking directories via unlink
      if ((curr->node->flags & FS_TYPE_MASK) == FS_DIRECTORY)
        return -1; // EISDIR

      // Unlink from the list
      if (prev)
        prev->next = curr->next;
      else
        dir->children = curr->next;

      // Free the file data if it's a ramfs file
      if (curr->node->device && (curr->node->flags & FS_TYPE_MASK) == FS_FILE) {
        ramfs_file_t *file = (ramfs_file_t *)curr->node->device;
        ramfs_free_file_data(file);
        kfree(file);
      }
      kfree(curr->node);
      kfree(curr);
      return 0;
    }
    prev = curr;
    curr = curr->next;
  }
  return -1; // Not found
}

static int ramfs_rmdir(vfs_node_t *node, char *name) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY || !node->device)
    return -1;
  ramfs_dir_t *dir = (ramfs_dir_t *)node->device;
  child_node_t *prev = NULL;
  child_node_t *curr = dir->children;
  while (curr) {
    if (strcmp(curr->node->name, name) == 0) {
      if ((curr->node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return -1;
      ramfs_dir_t *child_dir = (ramfs_dir_t *)curr->node->device;
      if (!child_dir || child_dir->children)
        return -1;
      if (prev)
        prev->next = curr->next;
      else
        dir->children = curr->next;
      kfree(child_dir);
      kfree(curr->node);
      kfree(curr);
      return 0;
    }
    prev = curr;
    curr = curr->next;
  }
  return -1;
}

// ramfs_rename: Rename a file within the same directory
static int ramfs_rename(vfs_node_t *node, char *old_name, char *new_name) {
  if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY || !node->device)
    return -1;

  // Check that new_name doesn't already exist
  if (ramfs_finddir(node, new_name) != 0) {
    // If target exists, unlink it first (overwrite semantics per POSIX)
    vfs_node_t *target = ramfs_finddir(node, new_name);
    if (target && (target->flags & FS_TYPE_MASK) != FS_DIRECTORY) {
      ramfs_unlink(node, new_name);
    } else {
      return -1; // Can't overwrite a directory
    }
  }

  vfs_node_t *child = ramfs_finddir(node, old_name);
  if (!child)
    return -1; // Source not found

  strncpy(child->name, new_name, 127);
  child->name[127] = '\0';
  return 0;
}

// Public APIs

void ramfs_init(void) {
  next_inode = 1;
  // Create the root directory
  vfs_node_t *root = ramfs_make_node("/", 0755, FS_DIRECTORY);
  // Bind APIs
  root->create = ramfs_create;
  root->mkdir = ramfs_mkdir;
  root->unlink = ramfs_unlink;
  root->rmdir = ramfs_rmdir;
  root->rename = ramfs_rename;
  root->mknod = ramfs_mknod;

  fs_root = root;

  // Automatically create /dev and /tmp for early boot
  // (ext2 will provide the real /dev and /tmp after mounting)
  ramfs_mkdir(fs_root, "dev", 0755);
  ramfs_mkdir(fs_root, "tmp", 0777);
}

void ramfs_mount_node(vfs_node_t *root, vfs_node_t *node) {
  if (!root || !node)
    return;
  if ((root->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return;

  if (ramfs_finddir(root, node->name) != NULL)
    return;

  ramfs_add_child(root, node);
}
void ramfs_mount_on(vfs_node_t *node) {
  if (!node)
    return;

  // Initialize a ramfs directory structure
  ramfs_dir_t *dir = kmalloc(sizeof(ramfs_dir_t));
  if (!dir)
    return;
  memset(dir, 0, sizeof(ramfs_dir_t));

  // Transform the existing node into a ramfs directory
  node->device = dir;
  node->flags = (node->flags & ~0x07) | FS_DIRECTORY | FS_PERSISTENT;

  node->read = 0;
  node->write = 0;
  node->readdir = ramfs_readdir;
  node->finddir = ramfs_finddir;
  node->create = ramfs_create;
  node->mkdir = ramfs_mkdir;
  node->unlink = ramfs_unlink;
  node->rmdir = ramfs_rmdir;
  node->rename = ramfs_rename;
  node->mknod = ramfs_mknod;
  node->chmod = ramfs_chmod;
  node->chown = ramfs_chown;
}

void ramfs_mount_at(char *path) {
  vfs_node_t *mountpoint = vfs_resolve_path(path);
  if (!mountpoint) {
    // If it doesn't exist, try to create it in the root (legacy fallback)
    if (fs_root && fs_root->mkdir) {
      fs_root->mkdir(fs_root, path + (path[0] == '/' ? 1 : 0), 0755);
      mountpoint = vfs_resolve_path(path);
    }
  }

  if (mountpoint) {
    vfs_node_t *ram_root = kmalloc(sizeof(vfs_node_t));
    vfs_node_init(ram_root);
    char *name = path;
    char *last_slash = 0;
    for (char *p = path; *p; p++)
      if (*p == '/')
        last_slash = p;
    if (last_slash)
      name = last_slash + 1;
    strncpy(ram_root->name, name, 127);

    ramfs_mount_on(ram_root);
    vfs_mount(mountpoint, ram_root);
    vfs_close(mountpoint);
  }
}
