#!/usr/bin/env python3
"""Synthetic sysfs verifies AD106 ownership/group/ROM launch exclusions."""
from pathlib import Path
import importlib.util,tempfile
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('nvboot',root/'scripts/nouveau-boot.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
assert m.GUEST_SLOT==3 and m.GUEST_GPU_BDF=='0000:00:03.0'
assert m.guest_devices('0000:01:00.0')==[
 'vfio-pci,host=0000:01:00.0,addr=03.0,multifunction=on,rombar=1',
 'vfio-pci,host=0000:01:00.1,addr=03.1']
root_port=m.guest_devices('0000:01:00.0',root_port=True)
assert root_port[0].startswith('pcie-root-port,id=nouveau-port,')
assert 'hotplug=off' in root_port[0] and 'pref64-reserve=32G' in root_port[0]
assert root_port[1:]==[
 'vfio-pci,host=0000:01:00.0,bus=nouveau-port,addr=00.0,multifunction=on,rombar=1',
 'vfio-pci,host=0000:01:00.1,bus=nouveau-port,addr=00.1']
assert m.guest_devices(None,root_port=True)==root_port[:1]
assert m.guest_devices(None)==[]
assert m.guest_devices('0000:01:00.0',gpu_only=True)==m.guest_devices('0000:01:00.0')[:1]
assert m.guest_devices('0000:01:00.0',root_port=True,gpu_only=True)==root_port[:2]
assert m.guest_devices(None,root_port=True,gpu_only=True)==root_port[:1]
assert m.guest_devices(None,gpu_only=True)==[]
record=dict(kernel_sha256='candidate',cpus=1,firmware={'files':['verified']},pcie_root_port=False,gpu_only=False)
ready=dict(record,status='pass',software=True)
assert m.readiness_matches(ready,record)
legacy_ready=dict(ready);legacy_ready.pop('pcie_root_port');legacy_ready.pop('gpu_only')
assert m.readiness_matches(legacy_ready,record)
for key in ('pcie_root_port','gpu_only'):
 changed=dict(record,**{key:True})
 assert not m.readiness_matches(ready,changed)
 assert not m.readiness_matches(legacy_ready,changed)
 assert m.readiness_matches(dict(ready,**{key:True}),changed)
for changed in (dict(ready,status='fail'),dict(ready,software=False),dict(ready,cpus=2),
                dict(ready,kernel_sha256='other'),dict(ready,firmware={})):
 assert not m.readiness_matches(changed,record)
with tempfile.TemporaryDirectory(prefix='nouveau-preflight-') as temp:
 base=Path(temp);sys=base/'sys';proc=base/'proc';proc.mkdir();(proc/'cpuinfo').write_text('flags : clflush sse2\n')
 pci=sys/'bus/pci/devices';pci.mkdir(parents=True);group=sys/'kernel/iommu_groups/12';(group/'devices').mkdir(parents=True)
 for function,device,cls in [('0','2803','030000'),('1','22bd','040300')]:
  bdf='0000:01:00.'+function;d=pci/bdf;d.mkdir()
  for name,value in [('vendor','0x10de'),('device','0x'+device),('class','0x'+cls),('boot_vga','0')]: (d/name).write_text(value)
  config=bytearray(64);config[:4]=(int(device,16)<<16|0x10de).to_bytes(4,'little');config[0x3d]=1
  (d/'config').write_bytes(config)
  (d/'iommu_group').symlink_to(group);(group/'devices'/bdf).symlink_to(d)
  (d/'driver').symlink_to(sys/'bus/pci/drivers/vfio-pci')
 igpu=pci/'0000:0e:00.0';igpu.mkdir()
 (igpu/'vendor').write_text('0x1002');(igpu/'device').write_text('0x164e')
 (sys/'bus/pci/drivers/amdgpu').mkdir(parents=True);(igpu/'driver').symlink_to(sys/'bus/pci/drivers/amdgpu')
 hostdrm=sys/'class/drm/card0';hostdrm.mkdir(parents=True);(hostdrm/'device').symlink_to(igpu)
 def check():return m.inspect('0000:01:00.0',sysroot=sys,procroot=proc)
 # The driver target must exist, just as real sysfs links do.
 (sys/'bus/pci/drivers/vfio-pci').mkdir(parents=True)
 assert check()['ready_for_vfio_launch']
 config_path=pci/'0000:01:00.0/config';good=config_path.read_bytes()
 bad=bytearray(good);bad[0x3d]=255;config_path.write_bytes(bad)
 assert any('interrupt pin 255' in x for x in check()['blockers'])
 config_path.write_bytes(b'\xff'*64);assert any('lost its identity' in x for x in check()['blockers'])
 config_path.write_bytes(good[:16]);assert not check()['ready_for_vfio_launch']
 config_path.write_bytes(good)
 primary=pci/'0000:01:00.0/boot_vga';primary.write_text('1');assert any('firmware primary' in x for x in check()['blockers']);primary.write_text('0')
 (igpu/'driver').unlink();assert any('Raphael' in x for x in check()['blockers']);(igpu/'driver').symlink_to(sys/'bus/pci/drivers/amdgpu')
 audio=pci/'0000:01:00.1';(audio/'driver').unlink();assert any('no driver' in x for x in check()['blockers']);(audio/'driver').symlink_to(sys/'bus/pci/drivers/vfio-pci')
 (group/'devices/0000:02:00.0').touch();assert any('unreviewed' in x for x in check()['blockers']);(group/'devices/0000:02:00.0').unlink()
 drm=sys/'class/drm/card1';drm.mkdir(parents=True);(drm/'device').symlink_to(pci/'0000:01:00.0');assert any('host DRM' in x for x in check()['blockers']);(drm/'device').unlink()
 process=proc/'123';process.mkdir();(process/'cmdline').write_bytes(b'/usr/bin/qemu-system-x86_64\0-device\0vfio-pci,host=0000:01:00.0\0');assert any('owners' in x for x in check()['blockers'])
 (process/'cmdline').write_bytes(b'/bin/sh\0-c\0echo qemu-system-x86_64 0000:01:00.0\0');assert check()['ready_for_vfio_launch']
 (process/'fd').mkdir();(process/'fd/9').symlink_to('/dev/vfio/12');assert any('owners' in x for x in check()['blockers']);(process/'fd/9').unlink()
 (proc/'cpuinfo').write_text('flags : sse2\n');assert any('CLFLUSH' in x for x in check()['blockers'])
 for bdf in ['01:00.0','0000:01:00.1','0000:0e:00.0']:
  try:m.inspect(bdf,sysroot=sys,procroot=proc)
  except (RuntimeError,OSError):pass
  else:raise AssertionError(bdf)
 rom=base/'rom';data=bytearray(512);data[:2]=b'\x55\xaa';data[0x18:0x1a]=(0x40).to_bytes(2,'little');data[0x40:0x48]=b'PCIR\xde\x10\x03\x28';rom.write_bytes(data);assert len(m.verify_rom(rom))==64
 data[0x44:0x46]=b'\x02\x10';rom.write_bytes(data)
 try:m.verify_rom(rom)
 except RuntimeError:pass
 else:raise AssertionError('accepted AMD ROM')
print('PASS: exact GPU/functions, group isolation, host DRM/driver/VM/group ownership, CLFLUSH and matching NVIDIA ROM')

with tempfile.TemporaryDirectory(prefix='nouveau-history-') as temp:
 directory=Path(temp);(directory/'result.json').write_text('{"status":"fail"}')
 (directory/'nouveau-hardware-host.log').write_text('invalid PCI interrupt pin 255')
 (directory/'root.img').write_bytes(b'large fixture omitted')
 m.archive_run(directory)
 saved=list((directory/'history').iterdir());assert len(saved)==1
 assert (saved[0]/'result.json').read_text()=='{"status":"fail"}'
 assert (saved[0]/'nouveau-hardware-host.log').is_file() and not (saved[0]/'root.img').exists()
print('PASS: previous result and host failure evidence archived without copying disk fixtures')

cleanup = ('Nouveau diagnostic unregister begin\n'
           'booter-unload: booted\n'
           'Nouveau diagnostic unregister complete\n')
m.verify_cleanup(cleanup)
shutdown_cleanup = ('Nouveau shutdown remove begin 0000:00:03.0\n'
                    'sec2(gsp):booter-unload: booted\n'
                    'Nouveau shutdown remove complete 0000:00:03.0\n')
m.verify_cleanup(shutdown_cleanup)
for invalid in (shutdown_cleanup.replace('booter-unload: booted', 'booter-unload: booting'),
                shutdown_cleanup.replace('Nouveau shutdown remove complete', 'incomplete'),
                'booter-unload: booted\nNouveau shutdown remove begin\n'
                'Nouveau shutdown remove complete\n'):
    try:
        m.verify_cleanup(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError('incomplete shutdown cleanup accepted')
for invalid in ('', cleanup.replace('booter-unload: booted', 'booter-unload: failed'),
                cleanup.replace('unregister complete', 'unregister incomplete'),
                'booter-unload: booted\nNouveau diagnostic unregister begin\n'
                'Nouveau diagnostic unregister complete\n'):
    try:
        m.verify_cleanup(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError('accepted incomplete or out-of-order cleanup')
print('PASS: cleanup result requires ordered unregister, successful GSP unload and completion')

baseline = 'NOUVEAU_PROBE: VFIO-only baseline exposes no Nouveau DRM card\n'
m.verify_vfio_only(baseline)
for invalid in ('', baseline+'[NOUVEAU] probe begin 0000:00:03.0\n',
                baseline+'[drm] Initialized nouveau 1.4.0\n',
                baseline+'NOUVEAU_PROBE: DRM registered /dev/dri/card2\n'):
    try:
        m.verify_vfio_only(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError('accepted Nouveau activity in the VFIO-only baseline')
print('PASS: VFIO-only baseline requires a closed driver gate and no Nouveau DRM registration')
