#!/usr/bin/env python3
"""C5 oracle uses the imported ABI and cannot reuse C4-only readiness."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nvboot', root/'scripts/nouveau-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
uapi = root/'build/nouveau-618/linux/include/uapi/drm'
if not uapi.is_dir():
    uapi = root/'kernel/linux/include/uapi/drm'
with tempfile.TemporaryDirectory(prefix='nouveau-c5-oracle-') as temp:
    binary = Path(temp)/'memory'
    subprocess.run(['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(uapi), str(root/'userland/test_nouveau_memory.c'),
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), '--software'], text=True, capture_output=True, check=True)
    boot.verify_c5_memory(result.stdout, software=True)
    assert 'mismatch offset=4096' in result.stderr
    assert subprocess.run([str(binary), '--unknown'], capture_output=True).returncode == 2

record = dict(kernel_sha256='kernel', cpus=1, firmware={'files':['matched']},
              pcie_root_port=True, gpu_only=True, oracle_kind='c5-memory',
              oracle_source_sha256='source', oracle_uapi_sha256={'drm.h':'abi','nouveau_drm.h':'abi'})
ready = dict(record, status='pass', software=True)
assert boot.readiness_matches(ready, record)
for key, value in [('oracle_kind','probe'), ('oracle_source_sha256','old'),
                   ('oracle_uapi_sha256',{'drm.h':'different'})]:
    assert not boot.readiness_matches(dict(ready, **{key:value}), record)
legacy = dict(ready)
for key in ('oracle_kind','oracle_source_sha256','oracle_uapi_sha256'):
    legacy.pop(key)
assert not boot.readiness_matches(legacy, record)
log = ('NOUVEAU_C5: START memory-vm\n'
       'NOUVEAU_C5: device=/dev/dri/renderD129 driver=nouveau pci=10de:2803\n'
       + 'NOUVEAU_C5: PASS GART bytes=16384\n'*2
       + 'NOUVEAU_C5: PASS VRAM bytes=65536\n'*2
       + 'NOUVEAU_C5: PASS VM_INIT/synchronous MAP/UNMAP/negative ranges/handle lifetime\n'
       'NOUVEAU_C5: END memory-vm status=pass\n')
boot.verify_c5_memory(log)
for invalid in ('', log.replace('NOUVEAU_C5: PASS VRAM ', 'skipped '),
                log.replace('NOUVEAU_C5: PASS GART bytes=16384\n','',1),
                log.replace('NOUVEAU_C5: END memory-vm status=pass','incomplete'),
                log+'NOUVEAU_C5: FAIL VM_BIND\n', result.stdout):
    try:
        boot.verify_c5_memory(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError('incomplete C5 result accepted')
print('PASS: imported UAPI, software corruption oracle, strict C5 result markers and source/ABI-qualified readiness')
