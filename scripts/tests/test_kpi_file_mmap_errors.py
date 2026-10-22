#!/usr/bin/env python3
"""Real Linux file mmap bridge preserves driver errors and failed-map ownership."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/linuxkpi/src/file.c').read_text()
start = source.index('unsigned long linuxkpi_file_mmap_file(void *opaque_file,')
brace = source.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#define GFP_KERNEL 0
#define PAGE_SHIFT 12
struct file;
struct vm_area_struct {
  unsigned long vm_start, vm_end, vm_pgoff, vm_flags, vm_page_prot;
  struct file *vm_file;
};
struct file_operations { int (*mmap)(struct file *, struct vm_area_struct *); };
struct file { const struct file_operations *f_op; int refs; };
struct kpi_mmap_bridge { struct vm_area_struct vma; int refs; bool closed; };
static struct kpi_mmap_bridge *pending;
static int allocated, map_calls, driver_error;
static bool fail_alloc;
static void *kzalloc(size_t size, unsigned int flags) {
  (void)flags;
  if (fail_alloc) return NULL;
  allocated++;
  return calloc(1, size);
}
static void kfree(void *memory) { allocated--; free(memory); }
static struct file *get_file(struct file *file) { file->refs++; return file; }
static void fput(struct file *file) { assert(file->refs > 0); file->refs--; }
static unsigned long kpi_prot_to_vm_flags(unsigned long prot, unsigned long flags) {
  return prot | flags;
}
#define __pgprot(value) (value)
static void linuxkpi_vma_set_pending(void *bridge) { assert(!pending); pending = bridge; }
static int mmap_driver(struct file *file, struct vm_area_struct *vma) {
  map_calls++;
  assert(file->refs == 2 && vma->vm_file == file);
  assert(vma->vm_start == 0x100000 && vma->vm_end == 0x104000);
  assert(vma->vm_pgoff == 0x100000);
  return driver_error;
}
'''
code += source[start:end] + '\n'
code += r'''
int main(void) {
  const struct file_operations operations = {.mmap = mmap_driver};
  struct file file = {.f_op = &operations, .refs = 1};
  assert((long)linuxkpi_file_mmap_file(NULL, 0, 0, 0, 0, 0) == -ENODEV);
  struct file no_operations = {.refs = 1};
  assert((long)linuxkpi_file_mmap_file(&no_operations, 0, 0, 0, 0, 0) == -ENODEV);
  const int errors[] = {-EACCES, -EINVAL, -ENOMEM, -EFAULT, -ENODEV, 1};
  for (unsigned int index = 0; index < sizeof(errors)/sizeof(errors[0]); index++) {
    driver_error = errors[index];
    long result = linuxkpi_file_mmap_file(&file, 0x100000, 0x4000, 3, 1, 0x100000000UL);
    assert(result == (driver_error < 0 ? driver_error : -EINVAL));
    assert(file.refs == 1 && !pending && allocated == 0);
  }
  fail_alloc = true;
  int calls = map_calls;
  assert((long)linuxkpi_file_mmap_file(&file, 0, 0, 0, 0, 0) == -ENOMEM);
  assert(map_calls == calls && file.refs == 1 && allocated == 0 && !pending);
  fail_alloc = false;
  driver_error = 0;
  assert(linuxkpi_file_mmap_file(&file, 0x100000, 0x4000, 3, 1, 0x100000000UL) == 0x100000);
  assert(pending && pending->refs == 1 && !pending->closed && file.refs == 2);
  fput(pending->vma.vm_file);
  kfree(pending);
  pending = NULL;
  assert(file.refs == 1 && allocated == 0);
  puts("PASS: Linux mmap errors survive the bridge; failures leave no VMA/file references; success retains its mapping reference");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-mmap-errors-') as temp:
    directory = Path(temp)
    (directory/'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(directory/'test.c'), '-o', str(directory/'test')], check=True)
    subprocess.run([str(directory/'test')], check=True)
