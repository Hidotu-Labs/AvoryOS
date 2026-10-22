#!/usr/bin/env python3
"""Exact page-release accounting excludes permanent lazy metadata, not leaks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/src/tests/linuxkpi/linux/test_phase2_page.c').read_text()
start = source.index('static bool test_page_leak_invariant(')
brace = source.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define GFP_KERNEL 1
#define __GFP_ZERO 2
#define klogf(...) ((void)0)
struct page { unsigned int order; bool live; };
static struct page pages[512];
static unsigned long free_pages, metadata_pages, live_pages;
static unsigned int calls, frees, fail_call, bad_free_call;
static int free_error;
static bool irq_enabled;
#define local_irq_save(flags) do { (flags) = irq_enabled; irq_enabled = false; } while (0)
#define local_irq_restore(flags) do { irq_enabled = (flags); } while (0)
static unsigned long asc_pmm_get_free_pages(void) { return free_pages; }
static struct page *alloc_pages(unsigned int gfp, unsigned int order) {
  (void)gfp;
  assert(irq_enabled && calls < 512);
  calls++;
  if (calls == fail_call)
    return NULL;
  struct page *page = &pages[calls - 1];
  page->order = order;
  page->live = true;
  live_pages += 1UL << order;
  free_pages -= (1UL << order) + 1;
  metadata_pages++;
  return page;
}
static void __free_pages(struct page *page, unsigned int order) {
  if (!page)
    return;
  assert(page->live && page->order == order);
  page->live = false;
  live_pages -= 1UL << order;
  frees++;
  free_pages += 1UL << order;
  if (frees == bad_free_call)
    free_pages += free_error;
}
static void reset(void) {
  memset(pages, 0, sizeof(pages));
  free_pages = 100000;
  metadata_pages = live_pages = 0;
  calls = frees = fail_call = bad_free_call = 0;
  free_error = 0;
  irq_enabled = true;
}
static bool legacy_invariant(void) {
  unsigned long before = asc_pmm_get_free_pages();
  for (unsigned int iteration = 0; iteration < 256; iteration++) {
    struct page *first = alloc_pages(GFP_KERNEL, iteration % 4);
    struct page *second = alloc_pages(GFP_KERNEL | __GFP_ZERO, 1);
    assert(first && second);
    __free_pages(first, iteration % 4);
    __free_pages(second, 1);
  }
  return asc_pmm_get_free_pages() == before;
}
'''
code += function + '\n'
code += r'''
int main(void) {
  reset();
  assert(!legacy_invariant());
  assert(live_pages == 0 && metadata_pages == 512);
  reset();
  assert(test_page_leak_invariant());
  assert(calls == 512 && frees == 512 && live_pages == 0);
  assert(free_pages + metadata_pages == 100000 && irq_enabled);
  for (unsigned int failed = 1; failed <= 512; failed++) {
    reset();
    fail_call = failed;
    assert(!test_page_leak_invariant());
    assert(live_pages == 0 && free_pages + metadata_pages == 100000);
    assert(irq_enabled);
  }
  for (unsigned int failed = 1; failed <= 512; failed++) {
    for (int error = -1; error <= 1; error += 2) {
      reset();
      bad_free_call = failed;
      free_error = error;
      assert(!test_page_leak_invariant());
      assert(irq_enabled && live_pages == 0);
    }
  }
  puts("PASS: lazy metadata is not a data leak; every under/over-free fails; every allocation-failure path releases its partner and restores IRQs");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-page-free-invariant-') as temp:
    directory = Path(temp)
    (directory/'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(directory/'test.c'), '-o', str(directory/'test')], check=True)
    subprocess.run([str(directory/'test')], check=True)
