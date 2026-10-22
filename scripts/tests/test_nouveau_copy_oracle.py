#!/usr/bin/env python3
"""GPU-copy encoding uses cached upstream class headers; no host GPU access."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nvboot', root/'scripts/nouveau-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
mesa = root/'build/nouveau-c0/mesa-source/mesa-26.2.5/src/nouveau'
includes = [root/'build/nouveau-618/linux/include/uapi/drm', mesa/'drm',
            mesa/'headers', mesa/'headers/nvidia/classes']
with tempfile.TemporaryDirectory(prefix='nouveau-c5-copy-') as temp:
    binary = Path(temp)/'copy'
    command = ['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror']
    for directory in includes:
        command += ['-I', str(directory)]
    subprocess.run(command+[str(root/'userland/test_nouveau_copy.c'), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), '--software'], text=True, capture_output=True, check=True)
    boot.verify_c5_copy(result.stdout, software=True)
    assert subprocess.run([str(binary), '--unknown'], capture_output=True).returncode == 2
log = ('NOUVEAU_C5_COPY: START execution\npci=10de:2803\n'
       + 'NOUVEAU_C5_COPY: PASS EXEC/fence GART->VRAM->GART bytes=8256 seed=17 canaries intact\n'*2
       + 'NOUVEAU_C5_COPY: PASS async VM_BIND point=1 -> EXEC wait=1 signal=2 timeline wait/query/future timeout\n'
       + 'NOUVEAU_C5_COPY: END execution status=pass\n')
boot.verify_c5_copy(log)
for invalid in (result.stdout, log.replace('GART->VRAM->GART','cpu memcpy'),
                log.replace('END execution status=pass','incomplete'),
                log+'NOUVEAU_C5: FAIL bounded GPU fence wait\n'):
    try:
        boot.verify_c5_copy(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError('unverified GPU execution accepted')
record = dict(kernel_sha256='kernel', cpus=1, firmware={}, oracle_kind='c5-copy',
              oracle_source_sha256='copy-and-memory-source', oracle_uapi_sha256={'classes':'matching'})
ready = dict(record, software=True, status='pass')
assert boot.readiness_matches(ready, record)
for changed in (dict(ready,oracle_kind='c5-memory'), dict(ready,oracle_uapi_sha256={}),
                dict(ready,oracle_source_sha256='changed')):
    assert not boot.readiness_matches(changed, record)
print('PASS: copy push methods/canaries, bounded execution markers and matching source/class-header readiness')
