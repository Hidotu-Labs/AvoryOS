#ifndef HEAP_H
#define HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Initialize the kernel heap allocator
void heap_init(void);

// Allocate 'size' bytes of memory
void *kmalloc(size_t size);

// Free an allocated block of memory
void kfree(void *ptr);

// Allocate 'num' elements of 'size' bytes and zero-initialize them
void *kcalloc(size_t num, size_t size);

// Reallocate a block to a 'new_size'. If 'ptr' is NULL, acts as kmalloc.
// If 'new_size' is 0, acts as kfree.
void *krealloc(void *ptr, size_t new_size);

// Get heap statistics formatted as a string
void heap_get_info(char *buf);

#endif
