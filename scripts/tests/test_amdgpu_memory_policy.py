#!/usr/bin/env python3
"""Compile the owned ioctl entry point and check delegation and ABI handling."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='avory-amdgpu-policy-') as tmp:
    p = Path(tmp)
    (p/'amdgpu.h').write_text('''#include <stdbool.h>
#include <stdint.h>
typedef uint64_t u64;
struct drm_gem_object {u64 size;};
static inline struct drm_gem_object *drm_gem_object_lookup(void *f,unsigned h){(void)f;(void)h;return 0;}
static inline void drm_gem_object_put(struct drm_gem_object *p){(void)p;}
#define gem_to_amdgpu_bo(p) (p)
struct drm_device { int unused; };
struct drm_file { int unused; };
struct amdgpu_device { struct drm_device dev; struct { bool buffer_funcs_enabled; } mman; };
#define drm_to_adev(dev) ((struct amdgpu_device *)(dev))
#define AMDGPU_GEM_DOMAIN_VRAM 4
#define AMDGPU_GEM_CREATE_VRAM_CLEARED 8
union drm_amdgpu_gem_create {
 struct { u64 bo_size, alignment, domains, domain_flags; } in;
 struct { uint32_t handle, pad; } out;
};
''')
    (p/'amdgpu_gem.h').write_text('')
    (p/'amdgpu_object.h').write_text('')
    (p/'linuxkpi').mkdir()
    (p/'linuxkpi/gpu_diag.h').write_text('#define gpu_diag_resource_enabled() 0\n#define gpu_diag_record(k,o,a,b,c,d,r) ((void)(o),(void)(a),(void)(b),(void)(c),(void)(d),(void)(r))\n')
    (p/'test.c').write_text('''#include "amdgpu.h"
#include <assert.h>
int amdgpu_gem_create_ioctl(struct drm_device *, void *, struct drm_file *);
static u64 observed_flags;
static int result, calls;
static struct drm_file *expected_file;
int linuxkpi_amdgpu_gem_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file) {
 union drm_amdgpu_gem_create *args = data;
 assert(dev && file == expected_file);
 observed_flags = args->in.domain_flags;
 calls++;
 if (!result) args->out.handle = 73;
 return result;
}
int main(void) {
 struct amdgpu_device dev = {0};
 static struct drm_file file = {0};
 expected_file = &file;
 for (int enabled = 0; enabled < 2; enabled++)
  for (int vram = 0; vram < 2; vram++)
   for (int failure = 0; failure < 2; failure++) {
    union drm_amdgpu_gem_create args = {0};
    args.in.bo_size = 4096;
    args.in.domains = vram ? 4 : 2;
    args.in.domain_flags = 1;
    dev.mman.buffer_funcs_enabled = enabled;
    result = failure ? -22 : 0;
    int before = calls;
    assert(amdgpu_gem_create_ioctl(&dev.dev, &args, &file) == result);
    assert(calls == before + 1);
    assert(observed_flags == (u64)(1 | ((enabled && vram) ? 8 : 0)));
    assert(args.in.domain_flags == 1);
    if (!failure) assert(args.out.handle == 73);
   }
}
''')
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-DAVORYOS_AMDGPU_MEMORY_POLICY',
                    '-I'+tmp,str(root/'kernel/linuxkpi/src/amdgpu_memory.c'),
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
    subprocess.run(['cc','-nostdinc','-c',str(root/'kernel/linuxkpi/src/amdgpu_memory.c'),
                    '-o',str(p/'disabled.o')],check=True)
print('PASS: native AMD allocation policy, upstream error/handle forwarding, caller flags and disabled-driver build')
