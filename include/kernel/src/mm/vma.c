#include "vma.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "heap.h"
#include "slab_cache.h"

// Slab-accelerated allocation for VMA nodes
// Falls back to kmalloc if the vma_cache hasn't been created yet (early boot).
static inline struct vma *vma_node_alloc(void) {
  if (vma_cache)
    return (struct vma *)kmem_cache_alloc(vma_cache);
  return (struct vma *)kmalloc(sizeof(struct vma));
}

static inline void vma_node_free(struct vma *v) {
  if (vma_cache)
    kmem_cache_free(vma_cache, v);
  else
    kfree(v);
}

static inline void vma_file_ref(void *file_node) {
  if (file_node)
    vfs_node_ref((vfs_node_t *)file_node);
}

static inline void vma_file_unref(void *file_node) {
  if (file_node)
    vfs_close((vfs_node_t *)file_node);
}

static inline void vma_drop_file_ref(struct vma *v) {
  if (v && v->file_node)
    vma_file_unref(v->file_node);
}

// Helper macros
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static int get_height(struct vma *n) { return n ? n->height : 0; }

static uint64_t get_max_end(struct vma *n) { return n ? n->max_end : 0; }

static void update_node(struct vma *n) {
  if (!n)
    return;
  n->height = 1 + MAX(get_height(n->left), get_height(n->right));
  n->max_end = n->end;
  uint64_t max_left = get_max_end(n->left);
  uint64_t max_right = get_max_end(n->right);
  if (max_left > n->max_end)
    n->max_end = max_left;
  if (max_right > n->max_end)
    n->max_end = max_right;
}

static struct vma *right_rotate(struct vma *y) {
  struct vma *x = y->left;
  struct vma *T2 = x->right;

  x->right = y;
  y->left = T2;

  update_node(y);
  update_node(x);
  return x;
}

static struct vma *left_rotate(struct vma *x) {
  struct vma *y = x->right;
  struct vma *T2 = y->left;

  y->left = x;
  x->right = T2;

  update_node(x);
  update_node(y);
  return y;
}

static int get_balance(struct vma *n) {
  return n ? get_height(n->left) - get_height(n->right) : 0;
}

static struct vma *insert_node(struct vma *node, struct vma *new_node) {
  if (!node)
    return new_node;

  if (new_node->start < node->start)
    node->left = insert_node(node->left, new_node);
  else if (new_node->start > node->start)
    node->right = insert_node(node->right, new_node);
  else
    return node; // Duplicate overlapping start boundaries rejected natively
                 // inside vma_add earlier

  update_node(node);

  int balance = get_balance(node);

  // Left Left
  if (balance > 1 && new_node->start < node->left->start)
    return right_rotate(node);

  // Right Right
  if (balance < -1 && new_node->start > node->right->start)
    return left_rotate(node);

  // Left Right
  if (balance > 1 && new_node->start > node->left->start) {
    node->left = left_rotate(node->left);
    return right_rotate(node);
  }

  // Right Left
  if (balance < -1 && new_node->start < node->right->start) {
    node->right = right_rotate(node->right);
    return left_rotate(node);
  }

  return node;
}

static struct vma *min_value_node(struct vma *node) {
  struct vma *current = node;
  while (current->left != NULL)
    current = current->left;
  return current;
}

static struct vma *delete_node(struct vma *node, uint64_t start,
                               bool *deleted) {
  if (!node)
    return node;

  if (start < node->start)
    node->left = delete_node(node->left, start, deleted);
  else if (start > node->start)
    node->right = delete_node(node->right, start, deleted);
  else {
    // Node to delete found
    *deleted = true;

    if (!node->left || !node->right) {
      struct vma *temp = node->left ? node->left : node->right;
      if (!temp) {
        vma_drop_file_ref(node);
        vma_node_free(node);
        node = NULL;
      } else {
        struct vma *unlinked = node;
        node = temp;
        vma_drop_file_ref(unlinked);
        vma_node_free(unlinked);
      }
    } else {
      // Node with two children: Get the inorder successor (smallest in the
      // right subtree)
      struct vma *temp = min_value_node(node->right);

      // The deleted node's file reference goes away, while the successor's
      // reference moves into this node with the copied payload below.
      vma_drop_file_ref(node);

      // Copy the inorder successor's data to this node
      node->start = temp->start;
      node->end = temp->end;
      node->prot = temp->prot;
      node->flags = temp->flags;
      node->offset = temp->offset;
      node->file_size = temp->file_size;
      node->fd = temp->fd;
      node->file_node = temp->file_node;
      temp->file_node = NULL;

      // Delete the inorder successor
      node->right = delete_node(node->right, temp->start, deleted);
    }
  }

  if (!node)
    return node;

  update_node(node);
  int balance = get_balance(node);

  // Left Left
  if (balance > 1 && get_balance(node->left) >= 0)
    return right_rotate(node);

  // Left Right
  if (balance > 1 && get_balance(node->left) < 0) {
    node->left = left_rotate(node->left);
    return right_rotate(node);
  }

  // Right Right
  if (balance < -1 && get_balance(node->right) <= 0)
    return left_rotate(node);

  // Right Left
  if (balance < -1 && get_balance(node->right) > 0) {
    node->right = right_rotate(node->right);
    return left_rotate(node);
  }

  return node;
}

void vma_list_init(struct vma_list *list) {
  list->root = NULL;
  list->count = 0;
}

static void vma_destroy_recursive(struct vma *node) {
  if (!node)
    return;
  vma_destroy_recursive(node->left);
  vma_destroy_recursive(node->right);
  vma_drop_file_ref(node);
  vma_node_free(node);
}

void vma_list_destroy(struct vma_list *list) {
  vma_destroy_recursive(list->root);
  list->root = NULL;
  list->count = 0;
}

int vma_add(struct vma_list *list, uint64_t start, uint64_t end, uint64_t prot,
            uint64_t flags, int fd, uint64_t offset, void *file_node,
            uint64_t file_size) {
  if (vma_find_overlap(list, start, end)) {
    return -1; // Overlapping regions rejected
  }

  struct vma *new_node = vma_node_alloc();
  if (!new_node)
    return -1; // OOM

  new_node->start = start;
  new_node->end = end;
  new_node->max_end = end;
  new_node->prot = prot;
  new_node->flags = flags;
  new_node->offset = offset;
  new_node->file_size = file_size;
  new_node->fd = fd;
  new_node->file_node = file_node;
  vma_file_ref(file_node);
  new_node->height = 1;
  new_node->left = NULL;
  new_node->right = NULL;

  list->root = insert_node(list->root, new_node);
  list->count++;

  return 0; // Success
}

bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end) {
  bool overall_removed = false;
  struct vma *v;

  // Continuously find and process overlapping regions recursively
  while ((v = vma_find_overlap(list, start, end)) != NULL) {
    overall_removed = true;

    // Save current properties before deleting the structure
    uint64_t v_start = v->start;
    uint64_t v_end = v->end;
    uint64_t prot = v->prot;
    uint64_t flags = v->flags;
    int fd = v->fd;
    uint64_t offset = v->offset;
    uint64_t orig_file_size = v->file_size;
    void *vma_file_node = v->file_node;
    vma_file_ref(vma_file_node);

    bool deleted = false;
    list->root = delete_node(list->root, v->start, &deleted);
    list->count--;

    // Case 1: Complete overlap - node is entirely subsumed -> do not re-insert
    // anything

    // Case 2: Unmap from middle - split into two flanking regions
    if (start > v_start && end < v_end) {
      uint64_t len1 = start - v_start;
      uint64_t sub1 = MIN(orig_file_size, len1);

      uint64_t rel2 = end - v_start;
      uint64_t len2 = v_end - end;
      uint64_t sub2 = (orig_file_size > rel2) ? MIN(orig_file_size - rel2, len2) : 0;

      vma_add(list, v_start, start, prot, flags, fd, offset, vma_file_node, sub1);
      vma_add(list, end, v_end, prot, flags, fd, offset + rel2, vma_file_node, sub2);
    }
    // Case 3: Unmap from start - shrinking start boundary forward
    else if (start <= v_start && end > v_start && end < v_end) {
      uint64_t rel = end - v_start;
      uint64_t len = v_end - end;
      uint64_t sub = (orig_file_size > rel) ? MIN(orig_file_size - rel, len) : 0;

      vma_add(list, end, v_end, prot, flags, fd, offset + rel, vma_file_node, sub);
    }
    // Case 4: Unmap from end - shrinking end boundary backward
    else if (end >= v_end && start > v_start && start < v_end) {
      uint64_t len = start - v_start;
      uint64_t sub = MIN(orig_file_size, len);

      vma_add(list, v_start, start, prot, flags, fd, offset, vma_file_node, sub);
    }

    vma_file_unref(vma_file_node);
  }

  return overall_removed;
}

int vma_mprotect(struct vma_list *list, uint64_t start, uint64_t end,
                 uint64_t new_prot) {
  uint64_t curr = start;
  while (curr < end) {
    struct vma *v = vma_find(list, curr);
    if (!v) {
      // Gap found. standard mprotect returns ENOMEM in this case if it hits a
      // gap.
      v = vma_find_overlap(list, curr, end);
      if (!v)
        break;
      curr = v->start;
      continue;
    }

    uint64_t m_start = MAX(v->start, start);
    uint64_t m_end = MIN(v->end, end);

    // Save attributes
    uint64_t flags = v->flags;
    int fd = v->fd;
    uint64_t offset = v->offset;
    uint64_t orig_file_size = v->file_size;
    void *vma_file_node = v->file_node;
    uint64_t original_start = v->start;
    vma_file_ref(vma_file_node);

    uint64_t rel = m_start - original_start;
    uint64_t m_len = m_end - m_start;
    uint64_t sub_file_size = (orig_file_size > rel) ? MIN(orig_file_size - rel, m_len) : 0;

    // Remove the overlapping part. vma_remove handles splitting the original
    // VMA.
    vma_remove(list, m_start, m_end);

    // Re-insert with new prot. The offset must be adjusted based on where
    // this segment started relative to the original VMA.
    vma_add(list, m_start, m_end, new_prot, flags, fd,
            offset + rel, vma_file_node, sub_file_size);
    vma_file_unref(vma_file_node);

    curr = m_end;
  }
  return 0;
}

static struct vma *vma_find_recursive(struct vma *node, uint64_t addr) {
  if (!node)
    return NULL;
  if (addr >= node->start && addr < node->end)
    return node;
  if (addr < node->start)
    return vma_find_recursive(node->left, addr);
  return vma_find_recursive(node->right, addr);
}

struct vma *vma_find(struct vma_list *list, uint64_t addr) {
  if (!list)
    return NULL;
  return vma_find_recursive(list->root, addr);
}

static struct vma *vma_find_overlap_recursive(struct vma *node, uint64_t start,
                                              uint64_t end) {
  if (!node)
    return NULL;

  if (node->left && node->left->max_end > start) {
    struct vma *left_res = vma_find_overlap_recursive(node->left, start, end);
    if (left_res)
      return left_res;
  }

  if (start < node->end && end > node->start)
    return node;

  if (start >= node->max_end)
    return NULL;

  return vma_find_overlap_recursive(node->right, start, end);
}

struct vma *vma_find_overlap(struct vma_list *list, uint64_t start,
                             uint64_t end) {
  if (!list)
    return NULL;
  return vma_find_overlap_recursive(list->root, start, end);
}

static struct vma *vma_find_growdown_recursive(struct vma *node, uint64_t cr2,
                                               uint64_t max_limit) {
  if (!node)
    return NULL;

  struct vma *res = vma_find_growdown_recursive(node->left, cr2, max_limit);
  if (res)
    return res;

  if ((node->flags & MAP_GROWSDOWN) && cr2 < node->start &&
      cr2 >= node->start - max_limit) {
    return node;
  }

  return vma_find_growdown_recursive(node->right, cr2, max_limit);
}

struct vma *vma_find_growdown(struct vma_list *list, uint64_t cr2,
                              uint64_t max_limit) {
  return vma_find_growdown_recursive(list->root, cr2, max_limit);
}

static void clone_recursive(struct vma_list *dst, struct vma *node) {
  if (!node)
    return;
  vma_add(dst, node->start, node->end, node->prot, node->flags, node->fd,
          node->offset, node->file_node, node->file_size);
  clone_recursive(dst, node->left);
  clone_recursive(dst, node->right);
}

void vma_list_clone(struct vma_list *dst, struct vma_list *src) {
  vma_list_init(dst);
  clone_recursive(dst, src->root);
}

static void inorder_gather(struct vma *node, struct vma **array, int *index) {
  if (!node)
    return;
  inorder_gather(node->left, array, index);
  array[*index] = node;
  (*index)++;
  inorder_gather(node->right, array, index);
}

// Stack-local capacity for vma_find_gap and vma_merge_adjacent.
// Covers the vast majority of processes without any heap allocation.
// Falls back to kmalloc only when a process has more VMAs than this.
#define VMA_STACK_CAP 64

uint64_t vma_find_gap(struct vma_list *list, uint64_t length,
                      uint64_t base_addr, uint64_t limit_addr) {
  if (list->count == 0) {
    if (base_addr + length <= limit_addr)
      return base_addr;
    return 0;
  }

  // Fast path: use a stack-local array for the common case.
  // Avoids a kmalloc/kfree round-trip on every mmap syscall when the
  // process has fewer than VMA_STACK_CAP mappings.
  struct vma *stack_arr[VMA_STACK_CAP];
  struct vma **arr;
  bool heap_used = false;

  if (list->count <= VMA_STACK_CAP) {
    arr = stack_arr;
  } else {
    arr = kmalloc(sizeof(struct vma *) * list->count);
    if (!arr)
      return 0;
    heap_used = true;
  }

  int idx = 0;
  inorder_gather(list->root, arr, &idx);

  uint64_t current  = base_addr;
  uint64_t gap_start = 0;

  for (int i = 0; i < idx; i++) {
    if (arr[i]->end <= current)
      continue;

    if (arr[i]->start > current) {
      uint64_t gap = arr[i]->start - current;
      if (gap >= length) {
        gap_start = current;
        break;
      }
    }
    current = MAX(current, arr[i]->end);
  }

  if (heap_used)
    kfree(arr);

  if (gap_start != 0)
    return gap_start;

  if (current + length <= limit_addr)
    return current;

  return 0;
}

void vma_merge_adjacent(struct vma_list *list) {
  if (list->count < 2)
    return;

  // Single-pass O(n) merge replacing the old O(n²) while(merged) loop.
  //
  // Old approach: re-allocated the array and re-scanned the whole tree
  // after every single merge, giving O(n²) tree operations plus O(n)
  // heap allocations per call.
  //
  // New approach:
  //   1. Gather all nodes into a sorted array once — O(n).
  //   2. Walk the array linearly, accumulating merge runs in-place.
  //      When a pair is mergeable, extend the current run's end; when not,
  //      emit the accumulated region and start a new run.
  //   3. Destroy the tree and rebuild it from the merged array — O(n log n)
  //      total, same as before but with a constant factor of 1 instead of n.

  struct vma *stack_arr[VMA_STACK_CAP];
  struct vma **arr;
  bool heap_used = false;

  if (list->count <= VMA_STACK_CAP) {
    arr = stack_arr;
  } else {
    arr = kmalloc(sizeof(struct vma *) * list->count);
    if (!arr)
      return;
    heap_used = true;
  }

  int idx = 0;
  inorder_gather(list->root, arr, &idx);

  // Collect merged intervals into a flat temporary list.
  // Stores all fields needed to reconstruct each VMA after the merge.
  struct vma_merged_entry {
    uint64_t start, end, prot, flags, offset, file_size;
    int      fd;
    void    *file_node;
  };

  struct vma_merged_entry merged_stack[VMA_STACK_CAP];
  struct vma_merged_entry *merged;
  bool merged_heap = false;

  if (idx <= VMA_STACK_CAP) {
    merged = merged_stack;
  } else {
    merged = kmalloc(sizeof(*merged) * idx);
    if (!merged) {
      if (heap_used) kfree(arr);
      return;
    }
    merged_heap = true;
  }

  int m = 0; // number of output regions

  // Seed with the first entry — copy all fields.
  merged[0].start     = arr[0]->start;
  merged[0].end       = arr[0]->end;
  merged[0].prot      = arr[0]->prot;
  merged[0].flags     = arr[0]->flags;
  merged[0].offset    = arr[0]->offset;
  merged[0].file_size = arr[0]->file_size;
  merged[0].fd        = arr[0]->fd;
  merged[0].file_node = arr[0]->file_node;
  m = 1;

  for (int i = 1; i < idx; i++) {
    struct vma *cur              = arr[i];
    struct vma_merged_entry *prev = &merged[m - 1];

    // Only merge truly anonymous adjacent regions with identical prot and
    // flags.  ELF PT_LOAD VMAs deliberately use fd == -1 because they are
    // backed directly by a vfs_node rather than a process descriptor, so fd
    // alone cannot distinguish them from anonymous mappings.  Merging two
    // such file-backed VMAs loses the second region's offset/file_size; after
    // a RELRO mprotect this makes the dynamic loader see zero-filled init
    // arrays and can lead to an indirect call through address zero.
    // Additionally, we avoid merging GROWSDOWN VMAs (stack) to preserve their 
    // identity for the fault handler's growth logic.
    bool can_merge = (prev->end   == cur->start) &&
                     (prev->prot  == cur->prot)   &&
                     (prev->flags == cur->flags)   &&
                     (prev->file_node == NULL)      &&
                     (cur->file_node  == NULL)      &&
                     !((prev->flags | cur->flags) &
                       (MAP_GROWSDOWN | MAP_SYSV_SHM));

    if (can_merge) {
      prev->end = cur->end; // extend the current run
    } else {
      // Carry the VMA through unchanged — preserve every field.
      merged[m].start     = cur->start;
      merged[m].end       = cur->end;
      merged[m].prot      = cur->prot;
      merged[m].flags     = cur->flags;
      merged[m].offset    = cur->offset;
      merged[m].file_size = cur->file_size;
      merged[m].fd        = cur->fd;
      merged[m].file_node = cur->file_node;
      m++;
    }
  }

  // Only rebuild the tree if at least one merge actually happened.
  if (m < idx) {
    for (int i = 0; i < m; i++)
      vma_file_ref(merged[i].file_node);
    vma_list_destroy(list); // frees all nodes, resets root + count
    for (int i = 0; i < m; i++) {
      vma_add(list,
              merged[i].start, merged[i].end,
              merged[i].prot,  merged[i].flags,
              merged[i].fd,    merged[i].offset,
              merged[i].file_node, merged[i].file_size);
      vma_file_unref(merged[i].file_node);
    }
  }

  if (merged_heap) kfree(merged);
  if (heap_used)   kfree(arr);
}

static void vma_dump_recursive(struct vma *node) {
  if (!node)
    return;
  // Inorder traversal to print in address order
  vma_dump_recursive(node->left);

  klog_puts("  ");
  klog_hex64(node->start);
  klog_puts(" - ");
  klog_hex64(node->end);
  klog_puts(" ");

  // Permissions
  klog_puts((node->prot & 0x1) ? "r" : "-");
  klog_puts((node->prot & 0x2) ? "w" : "-");
  klog_puts((node->prot & 0x4) ? "x" : "-");
  klog_puts(" ");

  // Flags
  if (node->flags & MAP_SHARED)
    klog_puts("shared ");
  if (node->flags & MAP_PRIVATE)
    klog_puts("private ");
  if (node->flags & MAP_ANONYMOUS)
    klog_puts("anon ");
  if (node->flags & MAP_GROWSDOWN)
    klog_puts("stack ");

  if (node->fd != -1) {
    klog_puts("fd=");
    klog_uint64((uint64_t)node->fd);
    klog_puts(" off=");
    klog_hex64(node->offset);
  }

  klog_puts("\n");

  vma_dump_recursive(node->right);
}

void vma_dump(struct vma_list *list) {
  if (!list || !list->root) {
    klog_puts("  (none)\n");
    return;
  }
  vma_dump_recursive(list->root);
}
