#!/usr/bin/env python3
"""Boot-time Xorg profile selection with and without a passed-through AMD VGA."""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'config/xorg/avory-xorg-profile.sh'
NATIVE = ROOT / 'config/xorg/10-native.conf'
AMDGPU = ROOT / 'config/xorg/10-modesetting.conf'


def make_sysfs(devices):
    """devices: list of (bdf, vendor, class) tuples. Returns sysfs root path."""
    tmp = Path(tempfile.mkdtemp(prefix='avory-xorg-sys-'))
    devdir = tmp / 'bus' / 'pci' / 'devices'
    devdir.mkdir(parents=True)
    for bdf, vendor, cls in devices:
        d = devdir / bdf
        d.mkdir()
        (d / 'vendor').write_text(vendor)
        (d / 'class').write_text(cls)
        if vendor == '0x1002' and cls.startswith('0x03'):
            driver = tmp / 'bus/pci/drivers/amdgpu'
            driver.mkdir(parents=True, exist_ok=True)
            (d / 'driver').symlink_to(driver)
    return tmp


def run_selector(sysfs, alt, dest, extra_env=None):
    env = dict(os.environ)
    env.pop('AVORY_XORG_PROFILE', None)
    if extra_env:
        env.update(extra_env)
    result = subprocess.run(['sh', str(SCRIPT), str(sysfs), str(dest), str(alt)],
                            env=env, capture_output=True, text=True)
    return result


def check(devices, expected_profile, extra_env=None, with_alt=True):
    sysfs = make_sysfs(devices)
    tmp = Path(tempfile.mkdtemp(prefix='avory-xorg-dest-'))
    try:
        alt = tmp / 'alt'
        alt.mkdir()
        if with_alt:
            shutil.copy(str(NATIVE), alt / '10-modesetting-native.conf')
            shutil.copy(str(AMDGPU), alt / '10-modesetting-amdgpu.conf')
        dest = tmp / 'xorg.conf.d' / '10-modesetting.conf'
        result = run_selector(sysfs, alt, dest, extra_env)
        assert result.returncode == 0, (result.returncode, result.stderr)
        assert f'xorg-profile: {expected_profile}' in result.stdout, result.stdout
        if with_alt:
            expected_src = AMDGPU if expected_profile == 'amdgpu' else NATIVE
            assert dest.read_bytes() == expected_src.read_bytes(), 'dest mismatch'
        return result
    finally:
        shutil.rmtree(str(sysfs), ignore_errors=True)
        shutil.rmtree(str(tmp), ignore_errors=True)


BOCHS = ('0000:00:02.0', '0x1234', '0x030000')
VIRTIO = ('0000:00:06.0', '0x1af4', '0x030000')
AMD_VGA = ('0000:00:03.0', '0x1002', '0x030000')
AMD_AUDIO = ('0000:00:03.1', '0x1002', '0x040300')

# VFIO topology: passed-through AMD VGA plus VirtIO output -> AMD PRIME layout.
check([AMD_VGA, VIRTIO], 'amdgpu')
gated = make_sysfs([AMD_VGA, VIRTIO])
with tempfile.TemporaryDirectory(prefix='avory-xorg-gated-') as temporary:
    (gated/'bus/pci/devices'/AMD_VGA[0]/'driver').unlink()
    alternate = Path(temporary)/'alt'
    alternate.mkdir()
    shutil.copy(NATIVE, alternate/'10-modesetting-native.conf')
    result = run_selector(gated, alternate, Path(temporary)/'selected.conf')
    assert 'xorg-profile: native' in result.stdout
shutil.rmtree(gated)
# Plain `make run` topology: only the std VGA (Bochs) -> native llvmpipe layout.
check([BOCHS], 'native')
# AMD audio function alone (stays on the host) must not select the AMD layout.
check([AMD_AUDIO, VIRTIO], 'native')
# No PCI devices at all, and missing sysfs root -> safe native default.
check([], 'native')
check([BOCHS], 'native')
missing = Path(tempfile.mkdtemp(prefix='avory-xorg-nosys-'))
shutil.rmtree(str(missing))
check([], 'native')
# Explicit override wins over detection in both directions.
check([AMD_VGA, VIRTIO], 'native', extra_env={'AVORY_XORG_PROFILE': 'native'})
check([BOCHS], 'amdgpu', extra_env={'AVORY_XORG_PROFILE': 'amdgpu'})
# Missing alternates: keep whatever is baked, still exit 0 (never fail boot).
sysfs = make_sysfs([AMD_VGA])
tmp = Path(tempfile.mkdtemp(prefix='avory-xorg-noalt-'))
try:
    dest = tmp / '10-modesetting.conf'
    dest.write_text('baked-default')
    result = run_selector(sysfs, tmp / 'empty-alt', dest)
    assert result.returncode == 0, result.stderr
    assert dest.read_text() == 'baked-default', 'dest must be untouched'
finally:
    shutil.rmtree(str(sysfs), ignore_errors=True)
    shutil.rmtree(str(tmp), ignore_errors=True)

# Repo invariants: the native fallback must not force glamor (Xorg dies on
# card0 without a GPU driver), and the AMD layout must keep its PRIME shape.
native_text = NATIVE.read_text()
assert 'AccelMethod' in native_text and '"none"' in native_text, native_text
amdgpu_text = AMDGPU.read_text()
assert 'PCI:0:3:0' in amdgpu_text, amdgpu_text
assert 'VirtualDisplay' in amdgpu_text, amdgpu_text

print('PASS: Xorg profile auto-selection (VFIO/native/override/missing-alt) and profile invariants')

# A negotiated virtio_gpu renderer enables glamor; a 2D ascentdrm stays native.
with tempfile.TemporaryDirectory(prefix='avory-virgl-xorg-') as tmp:
    base = Path(tmp)
    sysfs = base/'sys'; sysfs.mkdir()
    alternate = base/'alt'; alternate.mkdir()
    shutil.copy(NATIVE, alternate/'10-modesetting-native.conf')
    virgl = ROOT/'config/xorg/10-virgl.conf'
    shutil.copy(virgl, alternate/'10-modesetting-virgl.conf')
    identify = base/'identify'
    identify.write_text('#!/bin/sh\nprintf "%s\\n" "/dev/dri/card0 virtio_gpu pci:0000:00:06.0 1 /dev/dri/renderD137"\n')
    identify.chmod(0o755)
    dest = base/'selected.conf'
    selected = run_selector(sysfs, alternate, dest, {'AVORY_DRM_IDENTIFY': str(identify)})
    assert 'xorg-profile: virgl' in selected.stdout, selected
    assert dest.read_bytes() == virgl.read_bytes()
    assert '"glamor"' in dest.read_text() and '"DRI" "3"' in dest.read_text()
    assert '"SWcursor" "true"' in dest.read_text()
print('PASS: negotiated virgl selects glamor/DRI3')
