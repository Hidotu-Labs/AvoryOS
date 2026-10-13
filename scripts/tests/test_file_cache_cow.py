#!/usr/bin/env python3
"""Exercise the real cache-cluster install loop after a racing mprotect."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/src/mm/vmm_fault.c').read_text()


def block(start):
    begin = source.index(start)
    opening = source.index('{', begin)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


flags = block('static uint64_t dp_build_flags(')
loop = block('for (uint32_t bi = 0; bi < batch_count; bi++)')
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define PAGE_SIZE 4096
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_SHARED 1
#define PAGE_FLAG_PRESENT 1
#define PAGE_FLAG_RW 2
#define PAGE_FLAG_USER 4
#define PAGE_FLAG_COW 512
#define PAGE_FLAG_REFAULT 1024
#define PAGE_FLAG_NX (1ULL << 63)
typedef struct { int unused; } vfs_node_t;
struct vma {
    uint64_t start, end, file_size, offset, prot, flags;
    void *file_node;
};
struct mm { int vmas; } mm;
struct thread { struct mm *mm; } thread = { &mm };
static struct vma mapping;
static bool have_vma, install_ok;
static unsigned installs, releases;
static uint64_t installed_flags;
static struct vma *vma_find(int *tree, uint64_t va) {
    (void)tree; (void)va; return have_vma ? &mapping : 0;
}
static void pmm_decref(void *phys) { (void)phys; releases++; }
static bool vmm_map_page_if_unmapped(uint64_t *pml4, uint64_t va,
                                   uint64_t phys, uint64_t bits) {
    (void)pml4; (void)va; (void)phys;
    installs++; installed_flags = bits; return install_ok;
}
'''
code += flags + r'''
static void cluster(vfs_node_t *node) {
    struct thread *current = &thread;
    uint64_t target_cr3 = 0, vma_start = 0x100000, vma_offset = 0;
    struct { uint64_t vpage, phys; } batch[] = {{0x101000, 0x400000}};
    uint32_t batch_count = 1;
''' + loop + r'''
}
static void reset(vfs_node_t *node, uint64_t prot, uint64_t flags) {
    mapping = (struct vma){ .start=0x100000, .end=0x102000,
                           .file_node=node, .prot=prot, .flags=flags };
    have_vma = install_ok = true;
    installs = releases = 0; installed_flags = 0;
}
int main(void) {
    vfs_node_t node, other;
    /* A read-only fault fills the cache while mprotect adds write access. */
    reset(&node, PROT_READ | PROT_WRITE, MAP_PRIVATE); cluster(&node);
    assert(installs == 1 && releases == 0);
    assert(!(installed_flags & PAGE_FLAG_RW));
    assert(installed_flags & PAGE_FLAG_COW);
    /* Read-only private pages remain protected; executable pages retain X. */
    reset(&node, PROT_READ | PROT_EXEC, MAP_PRIVATE); cluster(&node);
    assert(!(installed_flags & (PAGE_FLAG_RW | PAGE_FLAG_NX)));
    assert(installed_flags & PAGE_FLAG_COW);
    /* MAP_SHARED intentionally permits writes to the cache. */
    reset(&node, PROT_READ | PROT_WRITE, MAP_SHARED); cluster(&node);
    assert(installed_flags & PAGE_FLAG_RW);
    assert(!(installed_flags & PAGE_FLAG_COW));
    reset(&node, PROT_READ, MAP_PRIVATE); install_ok=false; cluster(&node);
    assert(installs == 1 && releases == 1);
    reset(&other, PROT_READ, MAP_PRIVATE); cluster(&node);
    assert(installs == 0 && releases == 1);
    reset(&node, PROT_READ, MAP_PRIVATE); mapping.offset=4096; cluster(&node);
    assert(installs == 0 && releases == 1);
    reset(&node, PROT_NONE, MAP_PRIVATE); cluster(&node);
    assert(installs == 0 && releases == 1);
    reset(&node, PROT_READ, MAP_PRIVATE); have_vma=false; cluster(&node);
    assert(installs == 0 && releases == 1);
}
'''
with tempfile.TemporaryDirectory(prefix='avory-cache-cow-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: private cache pages retain CoW across a racing mprotect')
