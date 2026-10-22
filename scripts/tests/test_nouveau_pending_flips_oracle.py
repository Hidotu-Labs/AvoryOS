#!/usr/bin/env python3
"""Pending-flip qualification requires selection, every update and teardown."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nvboot', root / 'scripts/nouveau-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
uapi = root / 'build/nouveau-618/linux/include/uapi'
mesa = root / 'build/nouveau-c0/mesa-source/mesa-26.2.5/src/nouveau'
with tempfile.TemporaryDirectory(prefix='nouveau-pending-') as temp:
    command = ['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
               '-DNOUVEAU_PENDING_FLIPS=1', '-iquote', str(uapi / 'linux'),
               '-iquote', str(root / 'kernel/linuxkpi/include/uapi')]
    for directory in (uapi / 'drm', mesa / 'drm', mesa / 'headers', mesa / 'headers/nvidia/classes'):
        command.extend(('-I', str(directory)))
    subprocess.run([*command, str(root / 'userland/test_nouveau_gpu_present.c'),
                    '-o', str(Path(temp) / 'oracle')], check=True)

markers = [
    'NOUVEAU_C6_PRESENT: START linear PRIME presentation',
    'driver=nouveau pci=10de:2803', 'busid=pci:0000:00:06.0 import=1',
    'NOUVEAU_C6_PRESENT: READY frame=0 width=1280',
    'NOUVEAU_C6_PRESENT: READY frame=1 width=1280',
    'NOUVEAU_C6_PRESENT: END presentation status=pass',
    'NOUVEAU_C6_GPU: START copy-engine producer with explicit timeline wait',
    'NOUVEAU_C6_GPU: PASS EXEC frame=0 bytes=4096000 timeline=1 future=ETIME destination_cpu_unmapped=1',
    'NOUVEAU_C6_GPU: PASS EXEC frame=1 bytes=4096000 timeline=2 future=ETIME destination_cpu_unmapped=1',
    'NOUVEAU_C6_GPU: END GPU-produced presentation status=pass',
    'NOUVEAU_C6_FLIP: START queued EXEC dependency -> nonblocking implicit-fence flip',
    'NOUVEAU_C6_SELECT: PASS real inventory, paired roles, overrides and unavailable-device fallback hardware=1',
    'NOUVEAU_C6_FLIP: PASS pending RMFB/handle/fd teardown, isolated event queue and fresh-client recovery',
    'NOUVEAU_C6_FLIP: END six verified flips and pending teardown status=pass',
] + [f'NOUVEAU_C6_FLIP: PASS event frame={frame} cookie=verified' for frame in range(6)] + [
    f'NOUVEAU_C6_FLIP: PENDING frame={frame} ioctl_us=5' for frame in range(7)]
boot.verify_c6_flips('\n'.join(markers))
for index in range(len(markers)):
    try:
        boot.verify_c6_flips('\n'.join(markers[:index] + markers[index + 1:]))
    except RuntimeError:
        pass
    else:
        raise AssertionError(f'incomplete qualification accepted: {markers[index]}')
print('PASS: pending-profile compilation and strict selection/update/teardown evidence gates')
