#!/usr/bin/env python3
"""GPU presentation cannot reuse CPU-only readiness or omit producer fences."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nvboot', root/'scripts/nouveau-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
uapi = root/'build/nouveau-618/linux/include/uapi'
mesa = root/'build/nouveau-c0/mesa-source/mesa-26.2.5/src/nouveau'
with tempfile.TemporaryDirectory(prefix='nouveau-c6-gpu-present-') as temp:
    binary = Path(temp)/'present'
    command = ['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror', '-iquote', str(uapi/'linux')]
    for directory in (uapi/'drm', mesa/'drm', mesa/'headers', mesa/'headers/nvidia/classes'):
        command.extend(('-I', str(directory)))
    subprocess.run([*command, str(root/'userland/test_nouveau_gpu_present.c'), '-o', str(binary)], check=True)
    software = subprocess.run([str(binary), '--software'], capture_output=True, text=True, check=True)
    boot.verify_c6_gpu_present(software.stdout, software=True)
    assert subprocess.run([str(binary), '--unknown'], capture_output=True).returncode == 2

base = ('NOUVEAU_C6_PRESENT: START linear PRIME presentation\n'
        'driver=nouveau pci=10de:2803\n'
        'busid=pci:0000:00:06.0 import=1\n'
        'NOUVEAU_C6_PRESENT: READY frame=0 width=1280\n'
        'NOUVEAU_C6_PRESENT: READY frame=1 width=1280\n'
        'NOUVEAU_C6_PRESENT: END presentation status=pass\n')
producer = ('NOUVEAU_C6_GPU: START copy-engine producer with explicit timeline wait\n'
            'NOUVEAU_C6_GPU: PASS EXEC frame=0 bytes=4096000 timeline=1 future=ETIME destination_cpu_unmapped=1\n'
            'NOUVEAU_C6_GPU: PASS EXEC frame=1 bytes=4096000 timeline=2 future=ETIME destination_cpu_unmapped=1\n'
            'NOUVEAU_C6_GPU: END GPU-produced presentation status=pass\n')
log = base + producer
boot.verify_c6_gpu_present(log)
for invalid in (base, software.stdout, log.replace('timeline=2','timeline=1'),
                log.replace('future=ETIME','future=success'),
                log.replace('destination_cpu_unmapped=1','destination_cpu_unmapped=0'),
                log.replace('NOUVEAU_C6_GPU: END GPU-produced presentation status=pass','missing'),
                log+'NOUVEAU_C5: FAIL timeout\n', log+'NOUVEAU_C5_COPY: FAIL execute\n',
                log+'NOUVEAU_C6_PRIME: FAIL cleanup\n'):
    try:
        boot.verify_c6_gpu_present(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError('CPU-only, incomplete or failed producer accepted')
record = dict(kernel_sha256='kernel', cpus=1, firmware={'files':['matched']},
              pcie_root_port=True, gpu_only=True, oracle_kind='c6-gpu-present',
              oracle_source_sha256='all-cpu-gpu-capture-sources', oracle_uapi_sha256={'classes':'abi'})
ready = dict(record, status='pass', software=True)
assert boot.readiness_matches(ready, record)
for key, value in [('oracle_kind','c6-present'), ('oracle_source_sha256','old-copy-code'),
                   ('oracle_uapi_sha256',{'classes':'different'})]:
    assert not boot.readiness_matches(dict(ready, **{key:value}), record)
for args in (['--c6-gpu-present','--c6-present'], ['--c6-gpu-present','--vfio-only'],
             ['--c6-gpu-present','--qemu-extra=-qmp stdio']):
    assert subprocess.run(['python3', str(root/'scripts/nouveau-boot.py'), *args], capture_output=True).returncode == 2
source = (root/'userland/test_nouveau_gpu_present.c').read_text()
assert source.count('= mmap(') == 2
assert 'objects[0].info.map_handle' in source and 'objects[2].info.map_handle' in source
assert 'objects[1].info.map_handle' not in source
assert source.index('DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT') < source.index('DRM_IOCTL_MODE_SETCRTC')
print('PASS: GPU compilation/encoding, two producer timelines/future timeouts, no destination userspace map, explicit wait before presentation and isolated readiness')
