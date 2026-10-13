#!/usr/bin/env python3
"""Publication transport cannot invoke the task-context flip/vblank path."""
from pathlib import Path
import subprocess,sys
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/drivers/gpu/virtio_gpu/virtio_gpu.c').read_text()
transport=s[s.index('static uint64_t gpu_sync_command_pending;'):s.index('static struct pci_device *find_gpu(')]
assert 'vblank_wait_next' not in transport and 'virtio_gpu_complete_flip' not in transport
assert 'handle_async_completion' not in s
commit=s[s.index('static void virtio_gpu_commit_damage('):s.index('static uint32_t gpu_append_dec(')]
assert commit.index('ascentdrm_gem_end_cpu') < commit.index('vgpu_scanout_publish')
subprocess.run([sys.executable,str(root/'scripts/tests/test_prime_scanout_lifetime.py')],check=True)
print('PASS: command reaping cannot wait for vblank; publication releases producer reservation first')
