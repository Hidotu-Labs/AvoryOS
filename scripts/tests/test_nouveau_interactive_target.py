#!/usr/bin/env python3
"""The user's launch command must boot the installed system with plain QEMU."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
legacy=subprocess.check_output(['make','-n','run-nouevau-c4'],cwd=root,text=True)
start=legacy.index('\nqemu-system-x86_64 \\\n')
launch=legacy[start:]
assert '-cdrom avoryos-x86_64.iso' in launch
assert '-drive file=disk.img,format=raw,if=none,id=nvme0' in launch
assert 'host=0000:01:00.0,addr=03.0,multifunction=on' in launch
assert 'host=0000:01:00.1,addr=03.1' in launch
assert '-smp 8' in launch and '-device usb-kbd' in launch and '-device usb-mouse' in launch
assert 'path=build/logs/nouveau-c4.log,append=on' in launch
assert 'python' not in launch and 'romfile=' not in launch and 'snapshot=on' not in launch
assert 'init=/sbin/init splash quiet kpi_amdgpu=0 kpi_nouveau=1' in legacy
assert 'nouveau.gsp_kvm_passthrough=1' in legacy and 'KPI_NOUVEAU=1' in legacy

candidate=subprocess.check_output(['make','-n','run-nouveau-c4'],cwd=root,text=True)
candidate_launch=candidate[candidate.index('\nqemu-system-x86_64 \\\n'):]
assert '-cdrom avoryos-x86_64.iso' in candidate
assert '-drive file=disk.img,format=raw,if=none,id=nvme0' in candidate
assert 'host=0000:01:00.0,bus=nouveau-port,addr=00.0,multifunction=on' in candidate_launch
assert 'host=0000:01:00.1' not in candidate_launch
assert 'pcie-root-port,id=nouveau-port,bus=pcie.0,addr=03.0,chassis=1,hotplug=off,pref64-reserve=32G' in candidate_launch
assert candidate_launch.index('-device pcie-root-port')<candidate_launch.index('-device vfio-pci')
assert '-global q35-pcihost.pci-hole64-size=64G' in candidate_launch
assert '-device intel-hda -device hda-duplex,audiodev=snd0' in candidate_launch
assert '/sys/bus/pci/devices/0000:01:00.1/driver' in candidate
assert "cmp -s - build/nouveau-618/desktop-cmdline" in candidate
assert '-smp 8' in candidate_launch and '-device usb-kbd' in candidate and '-device usb-mouse' in candidate
assert 'logfile=build/logs/nouveau-c4.log,logappend=on' in candidate
assert 'path=build/logs/nouveau-c4-serial.sock,server=on,wait=off' in candidate
single = subprocess.check_output(['make','-n','run-nouveau-c4','NOUVEAU_SMP=1'],cwd=root,text=True)
single_launch = single[single.index('\nqemu-system-x86_64 \\\n'):]
assert '-smp 1' in single_launch
assert 'virtio_gpu.present_batch=1' in candidate
assert 'nouveau.debug=info drm.debug=0 ' in candidate
assert 'gpu_diag=0 gpu_perf=1' in candidate
diagnostic = subprocess.check_output(['make','-n','run-nouveau-c4',
    'NOUVEAU_DEBUG=info,gsp=debug','NOUVEAU_DRM_DEBUG=0x4',
    'NOUVEAU_GPU_DIAG=1','NOUVEAU_PERF=0'],cwd=root,text=True)
assert 'nouveau.debug=info,gsp=debug drm.debug=0x4' in diagnostic
assert 'gpu_diag=1 gpu_perf=0' in diagnostic
unbatched = subprocess.check_output(['make','-n','run-nouveau-c4','NOUVEAU_PRESENT_BATCH=0'],cwd=root,text=True)
assert 'virtio_gpu.present_batch=0' in unbatched
assert 'python3 scripts/' not in candidate and 'snapshot=on' not in candidate
makefile = (root/'GNUmakefile').read_text()
cmdline = next(line.partition(':=')[2].strip() for line in makefile.splitlines()
               if line.startswith('NOUVEAU_618_CMDLINE :='))
assert 'kpi_nouveau_bdf=0000:01:00.0' in cmdline
assert 'kpi_nouveau_remove_on_shutdown=1' in cmdline
assert 'kpi_nouveau_remove_on_shutdown=1' in candidate
assert 'avoryos-x86_64.iso' in makefile
assert "printf '%s\\n' '$(NOUVEAU_618_CMDLINE)' > build/nouveau-618/desktop-cmdline" in makefile
assert 'nouveau-618-desktop-firmware: kernel $(RUN_DISK_DEP)' in makefile
assert 'scripts/nouveau-618-build.py' not in makefile
assert 'VFIO_GUEST_ADDRESS ?= addr=03.0' in makefile
assert 'VFIO_PREDEVICES ?=' in makefile
assert 'pcie-root-port,id=nouveau-port' not in launch
print('PASS: 6.18 C4 preserves GPU-only topology, desktop disk, emulated audio and serial; defaults to eight vCPUs with a one-vCPU override; legacy topology unchanged')
