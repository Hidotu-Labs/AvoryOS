#!/usr/bin/env python3
"""PRIME sharing readiness must not be confused with presentation evidence."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nvboot', root/'scripts/nouveau-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
uapi = root/'build/nouveau-618/linux/include/uapi'
with tempfile.TemporaryDirectory(prefix='nouveau-c6-prime-') as temp:
    binary = Path(temp)/'prime'
    subprocess.run(['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(uapi/'drm'), '-iquote', str(uapi/'linux'),
                    str(root/'userland/test_nouveau_prime.c'), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), '--software'], text=True, capture_output=True, check=True)
    boot.verify_c6_prime(result.stdout, software=True)
    assert subprocess.run([str(binary), '--unknown'], capture_output=True).returncode == 2
record = dict(kernel_sha256='kernel', cpus=1, firmware={'files':['matched']},
              pcie_root_port=True, gpu_only=True, oracle_kind='c6-prime',
              oracle_source_sha256='source', oracle_uapi_sha256={'linux/dma-buf.h':'abi'})
ready = dict(record, status='pass', software=True)
assert boot.readiness_matches(ready, record)
for key, value in [('oracle_kind','c6-inventory'), ('oracle_source_sha256','old'),
                   ('oracle_uapi_sha256',{'linux/dma-buf.h':'different'})]:
    assert not boot.readiness_matches(dict(ready, **{key:value}), record)
log = ('NOUVEAU_C6_PRIME: START sharing\n'
       'NOUVEAU_C5: device=/dev/dri/renderD129 driver=nouveau pci=10de:2803\n'
       'NOUVEAU_C6_PRIME: sink=/dev/dri/card0 driver=ascentdrm busid=pci:0000:00:06.0 import=1\n'
       + 'NOUVEAU_C6_PRIME: PASS Nouveau->VirtIO GART bytes=32768 alias/reimport/reexport/lifetime; presentation not tested\n'*2
       + 'NOUVEAU_C6_PRIME: END sharing status=pass\n')
boot.verify_c6_prime(log)
for invalid in (result.stdout, log.replace('END sharing status=pass','incomplete'),
                log.replace('busid=pci:0000:00:06.0 import=1','wrong sink'),
                log.replace('NOUVEAU_C6_PRIME: PASS Nouveau->VirtIO GART bytes=32768 ', 'skipped ', 1),
                log+'NOUVEAU_C6_PRIME: FAIL teardown\n', log+'NOUVEAU_C5: FAIL pattern\n'):
    try:
        boot.verify_c6_prime(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError('incomplete or failed PRIME sharing accepted')
for args in (['--c6-prime', '--c6-inventory'], ['--c6-prime', '--c5-copy'], ['--c6-prime', '--vfio-only']):
    assert subprocess.run(['python3', str(root/'scripts/nouveau-boot.py'), *args], capture_output=True).returncode == 2
print('PASS: PRIME oracle compilation, software pattern check, strict two-client/sink markers and source/ABI qualification')
