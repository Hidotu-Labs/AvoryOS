#ifndef SLAB_CACHE_H
#define SLAB_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

//  Named Object Slab Cache Allocator
//
//  Provides O(1) allocation/free for fixed-size kernel objects with:
//   - Per-type object caches (struct thread, vfs_node_t, struct vma, etc.)
//   - Per-slab bitmapped free tracking (128-object capacity per slab page)
//   - Partial → Full → Free slab state machine
//   - Optional constructor/destructor callbacks for complex objects
//   - Global registry for introspection and statistics

#define SLAB_CACHE_NAME_MAX 32
#define SLAB_CACHE_MAX_CACHES 32
#define SLAB_OBJ_MAGIC 0xCAC4E0B1ECULL

struct slab_page;

// Object cache descriptor
typedef struct kmem_cache {
    char name[SLAB_CACHE_NAME_MAX];
    size_t obj_size;        // Size of each object (user-visible)
    size_t obj_aligned;     // obj_size rounded up to alignment
    size_t alignment;       // Minimum alignment for objects
    uint32_t objs_per_slab; // Number of objects that fit in a slab page

    // Slab lists (state machine: free → partial → full)
    struct slab_page *slabs_partial;
    struct slab_page *slabs_full;
    struct slab_page *slabs_free;

    // Optional constructor/destructor
    void (*ctor)(void *obj);
    void (*dtor)(void *obj);

    // Statistics
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t active_objects;
    uint64_t total_slabs;
} kmem_cache_t;

// Per-slab metadata (lives at the start of each slab page)
struct slab_page {
    uint64_t magic;           // Validation magic
    struct slab_page *next;
    struct slab_page *prev;
    kmem_cache_t *cache;      // Back-pointer to owning cache
    uint32_t free_count;
    uint32_t total_count;
    uint32_t bitmap[4];       // 128 bits — tracks allocated objects
} __attribute__((aligned(64)));

// Global Cache Registry API

// Initialize the slab cache subsystem
void slab_cache_init(void);

// Create a new named object cache.
// name:      Human-readable name (max 31 chars)
// obj_size:  Size of each object in bytes
// alignment: Minimum alignment (0 = default 8-byte)
// ctor:      Optional object constructor (called on alloc), may be NULL
// dtor:      Optional object destructor (called on free), may be NULL
// Returns a cache handle, or NULL on failure.
kmem_cache_t *kmem_cache_create(const char *name, size_t obj_size,
                                 size_t alignment,
                                 void (*ctor)(void *),
                                 void (*dtor)(void *));

// Destroy a cache. All objects MUST have been freed first.
void kmem_cache_destroy(kmem_cache_t *cache);

// Allocate one object from the cache. O(1) typical case.
void *kmem_cache_alloc(kmem_cache_t *cache);

// Free one object back to the cache. O(1).
void kmem_cache_free(kmem_cache_t *cache, void *obj);

// Shrink a cache: release completely empty slabs back to PMM.
void kmem_cache_shrink(kmem_cache_t *cache);

// Introspection

// Print statistics for all registered caches (serial/klog)
void kmem_cache_print_all(void);

// Get number of active objects across all caches
uint64_t kmem_cache_total_active(void);

// Pre-built Kernel Object Caches

// These are initialized by slab_cache_init() and used throughout the kernel.
extern kmem_cache_t *thread_cache;    // struct thread
extern kmem_cache_t *vfs_node_cache;  // vfs_node_t
extern kmem_cache_t *vma_cache;       // struct vma

#endif // SLAB_CACHE_H
