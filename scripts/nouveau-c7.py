"""Build and fingerprint the isolated C7 Vulkan/Zink shader diagnostic."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ('/usr/lib/libvulkan.so.1', '/usr/lib/libvulkan_nouveau.so',
           '/usr/lib/libvulkan_lvp.so', '/usr/lib/libGL.so.1', '/usr/lib/dri/zink_dri.so',
           '/usr/lib/gbm/dri_gbm.so')
ICDS = ('/usr/share/vulkan/icd.d/nouveau_icd.json', '/usr/share/vulkan/icd.d/lvp_icd.json')


def build(binary, tree, xorg=False):
    headers = ROOT / 'build/nouveau-c0/mesa-source/mesa-26.2.5/include'
    compiler = ROOT / 'toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc'
    subprocess.run([str(compiler), f'--sysroot={tree}', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(headers), str(ROOT / 'userland' / ('test_nouveau_xorg.c' if xorg else 'test_nouveau_zink.c')),
                    '-o', str(binary), '-lEGL', '-lGL', '-lvulkan', '-lX11',
                    '-lX11-xcb', '-lxcb', '-lxcb-dri3', '-lxcb-present'], check=True)
    spec = importlib.util.spec_from_file_location('c7_runtime', ROOT / 'scripts/image-runtime.py')
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)

    def source(guest):
        def readlink(path):
            host = tree / path.lstrip('/')
            return os.readlink(host) if host.is_symlink() else None
        return tree / runtime.resolve(guest, readlink).lstrip('/')

    fingerprints = {}
    pending = list(runtime_paths(tree, xorg)) + ['/usr/lib/libEGL.so.1', '/usr/lib/libEGL_mesa.so.0',
                               '/usr/lib/libgbm.so.1', '/usr/lib/dri/swrast_dri.so',
                               '/usr/lib/dri/radeonsi_dri.so', '/lib64/ld-linux-x86-64.so.2']
    for original in [binary]:
        dynamic = subprocess.check_output(['readelf', '-d', str(original)], text=True)
        pending.extend('/usr/lib/' + name for name in re.findall(r'\(NEEDED\).*\[([^]]+)\]', dynamic))
    while pending:
        guest = pending.pop()
        if guest in fingerprints:
            continue
        original = source(guest)
        fingerprints[guest] = hashlib.sha256(original.read_bytes()).hexdigest()
        dynamic = subprocess.check_output(['readelf', '-d', str(original)], text=True)
        for name in re.findall(r'\(NEEDED\).*\[([^]]+)\]', dynamic):
            for directory in ('/usr/lib/', '/lib/', '/lib64/'):
                if source(directory + name).is_file():
                    pending.append(directory + name)
                    break
            else:
                raise RuntimeError(f'C7 dependency missing: {name}')
    for path in ICDS:
        fingerprints[path] = hashlib.sha256(source(path).read_bytes()).hexdigest()
    for path in (headers / 'vulkan').glob('*.h'):
        fingerprints['headers/' + path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    files = {path: source(path) for path in ICDS}
    if xorg:
        keyboard = tree/'usr/share/xkeyboard-config-2'
        for path in sorted(keyboard.rglob('*')):
            if path.is_file() or path.is_symlink():
                guest = '/usr/share/X11/xkb/' + str(path.relative_to(keyboard))
                original = source('/' + str(path.relative_to(tree)))
                files[guest] = original
                fingerprints[guest] = hashlib.sha256(original.read_bytes()).hexdigest()
        files['/usr/lib/xorg/protocol.txt'] = tree/'usr/lib/xorg/protocol.txt'
        fingerprints['/usr/lib/xorg/protocol.txt'] = hashlib.sha256(files['/usr/lib/xorg/protocol.txt'].read_bytes()).hexdigest()
        identify = binary.parent / 'drm-identify'
        subprocess.run(['cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT/'build/nouveau-618/linux/include/uapi/drm'),
                        str(ROOT/'userland/drm_identify.c'), '-o', str(identify)], check=True)
        files.update({'/bin/drm-identify': identify,
                      '/etc/X11/xorg.conf': ROOT/'config/xorg/10-nouveau.conf',
                      '/usr/bin/avory-xorg-profile': ROOT/'config/xorg/avory-xorg-profile.sh',
                      '/usr/share/avory-xorg/10-modesetting-native.conf': ROOT/'config/xorg/10-native.conf',
                      '/usr/bin/avory-nouveau-xorg': ROOT/'config/xorg/avory-nouveau-xorg.sh',
                      '/usr/bin/avory-prime-display': ROOT/'config/xorg/avory-prime-display.sh',
                      '/usr/share/avory-xorg/10-modesetting-nouveau.conf': ROOT/'config/xorg/10-nouveau.conf'})
    return files, fingerprints


def runtime_paths(tree, xorg=False):
    if not xorg:
        return RUNTIME
    return RUNTIME + ('/usr/lib/Xorg', '/usr/bin/vkcube', '/usr/lib/libGLX_mesa.so.0', '/bin/sh',
                      '/usr/bin/awk', '/usr/bin/sed', '/usr/bin/xrandr', '/usr/bin/xkbcomp',
                      '/usr/bin/mktemp', '/usr/bin/cp', '/usr/bin/rm', '/usr/bin/mkdir') + tuple(
        '/' + str(path.relative_to(tree)) for path in sorted((tree/'usr/lib/xorg/modules').rglob('*.so')))


def verify(log, software=False):
    if re.search(r'gsp: rc engn:|errored - disabling channel|channel \d+ killed!', log):
        raise RuntimeError('C7 GPU channel fault or recovery event; rendering markers cannot qualify this run')
    markers = [f'NOUVEAU_C7: START mode={"software" if software else "hardware"}',
               'NOUVEAU_C7: PASS Vulkan device identity and instance teardown',
               'NOUVEAU_C7: END shader pixels and context restart status=pass']
    markers += [f'NOUVEAU_C7: PASS pixels cycle={cycle} frame={frame} pixels=4096'
                for cycle in range(3) for frame in range(4)]
    markers += [f'NOUVEAU_C7: PASS context teardown cycle={cycle}' for cycle in range(3)]
    if 'NOUVEAU_C7: FAIL' in log or any(marker not in log for marker in markers):
        raise RuntimeError('C7 Vulkan identity/shader pixels/context teardown incomplete')


def verify_xorg(log, software=False):
    verify(log, software)
    markers = (['NOUVEAU_C7_XORG: SOFTWARE profile compiled; no hardware Xorg claim'] if software else
               ['NOUVEAU_C7_XORG: END GLX shader pixels and PRIME presentation status=pass',
                'NOUVEAU_C7_XORG: STRESS updates=256 pixels=pass',
                'NOUVEAU_C7_XORG: VKCUBE frames=600 status=pass',
                'NOUVEAU_C7_XORG: STOP bounded=1 status=0'] +
               [f'NOUVEAU_C7_XORG: READY frame={frame} ' for frame in range(2)] +
               [f'NOUVEAU_C7: PASS pixels cycle=3 frame={frame} pixels=4096' for frame in range(4)])
    if any(marker not in log for marker in markers):
        raise RuntimeError('C7 Xorg GLX/PRIME startup, pixels or teardown incomplete')
