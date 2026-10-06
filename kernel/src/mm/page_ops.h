#ifndef PAGE_OPS_H
#define PAGE_OPS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
  PAGE_OPS_ENGINE_ERMS = 0,
  PAGE_OPS_ENGINE_NT = 1,
  PAGE_OPS_ENGINE_AVX2 = 2,
  PAGE_OPS_ENGINE_AVX512 = 3,
} page_ops_engine_t;

/* Low-level architecture assembly routines */
void page_zero_nt(void *page);
void page_copy_nt(void *dst, const void *src);
void page_zero_erms(void *page);
void page_copy_erms(void *dst, const void *src);
void page_zero_avx512(void *page);
void page_copy_avx512(void *dst, const void *src);
void page_zero_avx2(void *page);
void page_copy_avx2(void *dst, const void *src);

/* Dynamic dispatch function pointers */
extern void (*page_zero)(void *page);
extern void (*page_copy)(void *dst, const void *src);

/* 2 MB Huge Page operations */
void page_zero_huge(void *page);
void page_copy_huge(void *dst, const void *src);

/* Subsystem initialization & introspection */
void page_ops_init(void);
page_ops_engine_t page_ops_get_engine(void);
const char *page_ops_engine_name(page_ops_engine_t engine);

#endif /* PAGE_OPS_H */
