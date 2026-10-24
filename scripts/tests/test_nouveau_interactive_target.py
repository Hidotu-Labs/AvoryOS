#!/usr/bin/env python3
"""The single Nouveau launch target boots the verified desktop image."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
makefile = (root / 'GNUmakefile').read_text()
for retired in ('run-nouveau-c4', 'run-nouveau-c6-', 'run-nouveau-c7-',
                'run-nouevau-c4', 'run-nouveau-software', 'prepare-nouveau'):
    assert retired not in makefile, retired

launch = subprocess.check_output(['make', '-n', 'run-nouveau'], cwd=root, text=True)
qemu = launch[launch.index('\nqemu-system-x86_64 \\\n'):]
assert '-cdrom avoryos-x86_64.iso' in qemu
assert '-drive file=disk.img,format=raw,if=none,id=nvme0' in qemu
assert 'host=0000:01:00.0,bus=nouveau-port,addr=00.0,multifunction=on' in qemu
assert 'host=0000:01:00.1' not in qemu
assert 'pcie-root-port,id=nouveau-port,bus=pcie.0,addr=03.0,chassis=1,hotplug=off,pref64-reserve=32G' in qemu
assert qemu.index('-device pcie-root-port') < qemu.index('-device vfio-pci')
assert '-global q35-pcihost.pci-hole64-size=64G' in qemu
assert '-smp 8' in qemu and '-device usb-kbd' in qemu and '-device usb-mouse' in qemu
assert 'virtio-vga,addr=06.0,xres=1280,yres=800,edid=off' in qemu
assert 'path=build/logs/nouveau-c4-serial.sock,server=on,wait=off' in qemu
assert 'logfile=build/logs/nouveau-c4.log,logappend=on' in qemu
assert 'init=/sbin/init splash quiet kpi_amdgpu=1 kpi_nouveau=1' in launch
assert 'nouveau.gsp_kvm_passthrough' not in launch

single_cpu = subprocess.check_output(
    ['make', '-n', 'run-nouveau', 'NOUVEAU_SMP=1'], cwd=root, text=True)
assert '-smp 1' in single_cpu

edid = subprocess.check_output(
    ['make', '-n', 'run-nouveau', 'NOUVEAU_VIRTIO_EDID=on'], cwd=root, text=True)
assert 'virtio-vga,addr=06.0,xres=1280,yres=800,edid=on' in edid

diagnostic = subprocess.check_output([
    'make', '-n', 'run-nouveau', 'NOUVEAU_DEBUG=info,gsp=debug',
    'NOUVEAU_DRM_DEBUG=0x4', 'NOUVEAU_GPU_DIAG=1', 'NOUVEAU_PERF=0'],
    cwd=root, text=True)
assert 'nouveau.debug=info,gsp=debug drm.debug=0x4' in diagnostic
assert 'gpu_diag=1 gpu_perf=0' in diagnostic
print('PASS: run-nouveau is the only Nouveau desktop launcher and boots the verified ISO')
