#!/usr/bin/env python3
"""Prepare an isolated AD106 probe VM; never rebind or reset host PCI devices."""
from pathlib import Path
from types import SimpleNamespace
import argparse, datetime, fcntl, hashlib, importlib.util, json, re, shlex, shutil, subprocess, zlib, tempfile
ROOT=Path(__file__).resolve().parents[1]
GUEST_SLOT=3
GUEST_GPU_BDF=f'0000:00:{GUEST_SLOT:02x}.0'

def guest_devices(gpu, *, root_port=False, rom=None, gpu_only=False):
    devices=[]
    address=f'{GUEST_SLOT:02x}'
    bus=''
    if root_port:
        devices.append('pcie-root-port,id=nouveau-port,bus=pcie.0,addr=03.0,chassis=1,hotplug=off,pref64-reserve=32G')
        address='00'
        bus='bus=nouveau-port,'
    if gpu:
        devices.append(f'vfio-pci,host={gpu},{bus}addr={address}.0,multifunction=on,rombar=1'+(f',romfile={rom.absolute()}' if rom else ''))
        if not gpu_only:
            devices.append(f'vfio-pci,host={gpu[:-1]}1,{bus}addr={address}.1')
    return devices

def readiness_matches(ready, record):
    return (ready.get('status')=='pass' and bool(ready.get('software')) and
            all(ready.get(key)==record[key] for key in ('kernel_sha256','cpus','firmware')) and
            all(bool(ready.get(key))==bool(record.get(key)) for key in ('pcie_root_port','gpu_only')) and
            ready.get('oracle_kind','probe')==record.get('oracle_kind','probe') and
            (record.get('oracle_kind','probe')=='probe' or
             all(ready.get(key)==record.get(key) for key in ('oracle_source_sha256','oracle_uapi_sha256'))))

def load(name, file):
    spec=importlib.util.spec_from_file_location(name, ROOT/'scripts'/file)
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);return mod

def inspect(gpu, *, sysroot=Path('/sys'), procroot=Path('/proc')):
    if not re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.0',gpu):
        raise RuntimeError('explicit canonical display BDF ending in .0 required')
    pci=sysroot/'bus/pci/devices'; dev=pci/gpu
    if (int((dev/'vendor').read_text(),16),int((dev/'device').read_text(),16)) != (0x10de,0x2803):
        raise RuntimeError('first target must be the reviewed AD106 RTX 4060 Ti [10de:2803]')
    if int((dev/'class').read_text(),16)>>16 != 3: raise RuntimeError('target is not a display controller')
    functions=sorted(p.name for p in pci.glob(gpu[:-1]+'*'))
    expected=[gpu,gpu[:-1]+'1']
    if functions!=expected: raise RuntimeError(f'unreviewed PCI functions: {functions}; expected {expected}')
    rows=[];blockers=[]
    for bdf in functions:
        path=pci/bdf; group=path/'iommu_group'
        if not group.is_symlink(): raise RuntimeError(f'no IOMMU group: {bdf}')
        members=sorted(p.name for p in (group/'devices').iterdir())
        unknown=set(members)-set(functions)
        if unknown: blockers.append(f'{bdf} group has unreviewed devices: {sorted(unknown)}')
        driver=(path/'driver').resolve().name if (path/'driver').is_symlink() else None
        if driver!='vfio-pci':blockers.append(f'{bdf} bound to {driver or "no driver"}, requires vfio-pci')
        vendor=int((path/'vendor').read_text(),16);device=int((path/'device').read_text(),16)
        cls=int((path/'class').read_text(),16)
        if bdf.endswith('.1') and (vendor,device,cls>>8)!=(0x10de,0x22bd,0x0403):
            blockers.append(f'unreviewed audio function: {bdf}')
        header=None
        try:
            with (path/'config').open('rb') as stream: header=stream.read(64)
            if len(header)!=64 or int.from_bytes(header[:4],'little')!=(device<<16|vendor):
                blockers.append(f'{bdf} PCI configuration is unreadable or has lost its identity; stop retries and inspect host PCI state')
            elif header[0x3d]>4:
                blockers.append(f'{bdf} invalid PCI interrupt pin {header[0x3d]}; stop retries and cold boot the host before testing again')
        except OSError as error:
            blockers.append(f'{bdf} cannot read PCI configuration: {error}')
        state={}
        for name in ('power_state','reset_method','power/runtime_status',
                     'power/control','d3cold_allowed','current_link_speed','current_link_width'):
            try:state[name]=(path/name).read_text().strip()
            except OSError:pass
        rows.append(dict(bdf=bdf,vendor=hex(vendor),device=hex(device),pci_class=hex(cls),
                         driver=driver,group=group.resolve().name,members=members,
                         interrupt_pin=header[0x3d] if header and len(header)==64 else None,state=state))
    display=[]
    for card in (sysroot/'class/drm').glob('card[0-9]*'):
        if not re.fullmatch(r'card[0-9]+',card.name):continue
        if (card/'device').is_symlink():
            owner=(card/'device').resolve().name
            display.append(dict(card=card.name,device=owner))
            if owner in functions:blockers.append(f'host DRM node still belongs to NVIDIA: {card.name}')
    host_igpus=[]
    for path in pci.iterdir():
        if not (path/'vendor').is_file() or not (path/'device').is_file():continue
        if (int((path/'vendor').read_text(),16),int((path/'device').read_text(),16))!=(0x1002,0x164e):continue
        driver=(path/'driver').resolve().name if (path/'driver').is_symlink() else None
        host_igpus.append(dict(bdf=path.name,driver=driver))
    if not any(d['driver']=='amdgpu' and any(c['device']==d['bdf'] for c in display) for d in host_igpus):
        blockers.append('Raphael must be bound to host amdgpu with a DRM node before assigning NVIDIA')
    boot_vga=(dev/'boot_vga').read_text().strip()
    if boot_vga=='1':blockers.append('NVIDIA is still firmware primary display; select the iGPU in UEFI and reboot')
    owners=[]
    for process in procroot.iterdir():
        if not process.name.isdigit():continue
        try:command=(process/'cmdline').read_bytes()
        except OSError:continue
        if command.split(b'\0')[0].split(b'/')[-1].startswith(b'qemu-system-') and any(bdf.encode() in command for bdf in functions):
            owners.append(process.name)
        # An existing VFIO group owner may be a different virtualization tool.
        for fd in (process/'fd').glob('*'):
            try:target=str(fd.readlink())
            except OSError:continue
            if target in {f'/dev/vfio/{r["group"]}' for r in rows}: owners.append(process.name)
    if owners:blockers.append(f'existing VFIO/VM owners: {sorted(set(owners))}')
    cpu=(procroot/'cpuinfo').read_text()
    flags=[set(line.partition(':')[2].split()) for line in cpu.splitlines() if line.startswith('flags')]
    if not flags or any('clflush' not in f for f in flags):
        blockers.append('CLFLUSH is required: the inherited all-CPU WBINVD fallback is incomplete')
    return dict(gpu=gpu,functions=rows,host_drm=display,host_igpus=host_igpus,blockers=blockers,
                ready_for_vfio_launch=not blockers,boot_vga=boot_vga)

def verify_rom(path):
    data=path.read_bytes()
    if not 512<=len(data)<=4*1024*1024 or data[:2]!=b'\x55\xaa':raise RuntimeError('invalid PCI option ROM')
    off=int.from_bytes(data[0x18:0x1a],'little')
    if data[off:off+4]!=b'PCIR' or data[off+4:off+8]!=b'\xde\x10\x03\x28':
        raise RuntimeError('ROM PCIR identity must match NVIDIA 10de:2803; AMD VFCT ROMs are forbidden')
    return hashlib.sha256(data).hexdigest()

def archive_run(directory):
    """Retain small evidence files before reusing the large prepared fixture."""
    files=[p for p in directory.iterdir() if p.is_file() and
           (p.name in ('inputs.json','result.json','capture.json') or
            re.fullmatch(r'prime-frame-[0-9]+\.ppm',p.name) or p.name.endswith(('-serial.log','-host.log')))]
    if not files:return
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    destination=directory/'history'/stamp;destination.mkdir(parents=True)
    for path in files:shutil.copy2(path,destination/path.name)

def verify_firmware(directory):
    """Validate an explicit private bundle without changing the C0 firmware."""
    directory=directory.resolve()
    manifest=json.loads((directory/'manifest.json').read_text())
    entries=manifest['files']
    if len(entries)!=4:raise RuntimeError('expected the four AD106 GSP firmware files')
    version=manifest.get('gsp_interface')
    if version not in ('535.113.01','570.144'):raise RuntimeError('unreviewed GSP interface')
    names={f'nvidia/ad106/gsp/{name}-{version}.bin' for name in
           ('gsp','bootloader','booter_load','booter_unload')}
    if {entry['request'] for entry in entries}!=names:raise RuntimeError('unexpected firmware requests')
    root=directory/'lib/firmware'
    expected=set(names)
    if 'license_sha256' in manifest:
        notice=root/'LICENSES/LICENCE.nvidia'
        if not notice.resolve().is_relative_to(root):raise RuntimeError('license escapes bundle')
        if hashlib.sha256(notice.read_bytes()).hexdigest()!=manifest['license_sha256']:
            raise RuntimeError('firmware license integrity mismatch')
        expected.add('LICENSES/LICENCE.nvidia')
    if {str(p.relative_to(root)) for p in root.rglob('*') if p.is_file()}!=expected:
        raise RuntimeError('bundle contains extra or missing firmware')
    for entry in entries:
        path=root/entry['request']
        if not path.resolve().is_relative_to(root):raise RuntimeError('firmware escapes bundle')
        data=path.read_bytes()
        if not 0<len(data)<=64*1024*1024 or len(data)!=entry['size']:
            raise RuntimeError(f'firmware size mismatch: {path.name}')
        if hashlib.sha256(data).hexdigest()!=entry['sha256'] or zlib.crc32(data)!=entry['crc32']:
            raise RuntimeError(f'firmware integrity mismatch: {path.name}')
    return manifest

def verify_cleanup(log):
    for prefix, action in (('Nouveau shutdown', 'remove'),
                           ('Nouveau diagnostic', 'unregister')):
        begin=log.find(f'{prefix} {action} begin')
        if begin<0:continue
        unloaded=log.find('booter-unload: booted',begin)
        complete=log.find(f'{prefix} {action} complete',begin)
        if 0<=begin<unloaded<complete:return
    raise RuntimeError('Nouveau removal did not confirm GSP booter-unload before completing')

def verify_vfio_only(log):
    if ('NOUVEAU_PROBE: VFIO-only baseline exposes no Nouveau DRM card' not in log or
        '[NOUVEAU] probe begin' in log or '[drm] Initialized nouveau' in log or
        'NOUVEAU_PROBE: DRM registered' in log):
        raise RuntimeError('VFIO-only baseline did not demonstrate a closed Nouveau probe gate')

def verify_c5_memory(log, *, software=False):
    if software:
        markers=['NOUVEAU_C5: SOFTWARE pattern and corruption oracle pass; no GPU access']
    else:
        markers=['NOUVEAU_C5: START memory-vm','pci=10de:2803',
                 'NOUVEAU_C5: PASS GART ', 'NOUVEAU_C5: PASS VRAM ',
                 'NOUVEAU_C5: PASS VM_INIT/synchronous MAP/UNMAP/negative ranges/handle lifetime',
                 'NOUVEAU_C5: END memory-vm status=pass']
    repeated=software or all(log.count(f'NOUVEAU_C5: PASS {domain} ')>=2 for domain in ('GART','VRAM'))
    if not repeated or any(marker not in log for marker in markers) or 'NOUVEAU_C5: FAIL' in log:
        raise RuntimeError('C5 memory/VM oracle did not complete all required checks')

def verify_c5_copy(log, *, software=False):
    if software:
        markers=['NOUVEAU_C5_COPY: SOFTWARE push encoding and canaries pass; no GPU access']
    else:
        markers=['NOUVEAU_C5_COPY: START execution','pci=10de:2803',
                 'NOUVEAU_C5_COPY: PASS async VM_BIND point=1 -> EXEC wait=1 signal=2 timeline wait/query/future timeout',
                 'NOUVEAU_C5_COPY: END execution status=pass']
    repeated=software or log.count('NOUVEAU_C5_COPY: PASS EXEC/fence GART->VRAM->GART bytes=8256 ')>=2
    if not repeated or any(marker not in log for marker in markers) or any(marker in log for marker in ('NOUVEAU_C5: FAIL','NOUVEAU_C5_COPY: FAIL')):
        raise RuntimeError('C5 GPU execution did not verify copy bytes, canaries and fence completion across clients')

def verify_c6_inventory(log, *, software=False):
    if software:
        markers=['NOUVEAU_C6: SOFTWARE EDID validation and inventory bounds pass; no GPU access']
    else:
        markers=['NOUVEAU_C6: START inventory', 'driver=nouveau pci=10de:2803',
                 'NOUVEAU_C6: resources ', 'NOUVEAU_C6: connector=',
                 'NOUVEAU_C6: crtc=', 'NOUVEAU_C6: plane=',
                 'NOUVEAU_C6: PASS inventory connected=',
                 'NOUVEAU_C6: END inventory status=pass']
    if any(marker not in log for marker in markers) or 'NOUVEAU_C6: FAIL' in log:
        raise RuntimeError('C6 KMS inventory did not complete; no physical-output claim is implied')

def verify_c6_prime(log, *, software=False):
    if software:
        markers=['NOUVEAU_C6_PRIME: SOFTWARE pattern oracle pass; no GPU access']
    else:
        markers=['NOUVEAU_C6_PRIME: START sharing', 'driver=nouveau pci=10de:2803',
                 'busid=pci:0000:00:06.0 import=1',
                 'NOUVEAU_C6_PRIME: END sharing status=pass']
    repeated=software or log.count('NOUVEAU_C6_PRIME: PASS Nouveau->VirtIO GART bytes=32768 ')>=2
    if not repeated or any(marker not in log for marker in markers) or any(marker in log for marker in ('NOUVEAU_C6_PRIME: FAIL','NOUVEAU_C5: FAIL')):
        raise RuntimeError('C6 PRIME sharing did not verify cross-device bytes and lifetime; presentation remains separate')

def verify_c6_present(log, *, software=False):
    markers=(['NOUVEAU_C6_PRESENT: SOFTWARE two-frame pattern pass; no GPU access'] if software else
             ['NOUVEAU_C6_PRESENT: START linear PRIME presentation', 'driver=nouveau pci=10de:2803',
              'busid=pci:0000:00:06.0 import=1', 'NOUVEAU_C6_PRESENT: READY frame=0 ',
              'NOUVEAU_C6_PRESENT: READY frame=1 ', 'NOUVEAU_C6_PRESENT: END presentation status=pass'])
    if any(marker not in log for marker in markers) or any(marker in log for marker in ('NOUVEAU_C6_PRIME: FAIL','NOUVEAU_C5: FAIL')):
        raise RuntimeError('C6 PRIME presentation guest checks did not complete')

def verify_c6_gpu_present(log, *, software=False):
    verify_c6_present(log,software=software)
    markers=(['NOUVEAU_C6_GPU: SOFTWARE copy encoding and presentation pattern pass; no GPU access',
              'NOUVEAU_C5_COPY: SOFTWARE push encoding and canaries pass; no GPU access'] if software else
             ['NOUVEAU_C6_GPU: START copy-engine producer with explicit timeline wait',
              'NOUVEAU_C6_GPU: PASS EXEC frame=0 bytes=4096000 timeline=1 future=ETIME destination_cpu_unmapped=1',
              'NOUVEAU_C6_GPU: PASS EXEC frame=1 bytes=4096000 timeline=2 future=ETIME destination_cpu_unmapped=1',
              'NOUVEAU_C6_GPU: END GPU-produced presentation status=pass'])
    if any(marker not in log for marker in markers) or 'NOUVEAU_C5_COPY: FAIL' in log:
        raise RuntimeError('C6 GPU-produced presentation did not prove both EXEC jobs and producer timeline waits')

def verify_c6_flips(log, *, software=False):
    verify_c6_gpu_present(log,software=software)
    markers=(['NOUVEAU_C6_FLIP: SOFTWARE pending-producer/event profile compiled; no GPU access',
              'NOUVEAU_C6_SELECT: PASS real inventory, paired roles, overrides and unavailable-device fallback hardware=0'] if software else
             ['NOUVEAU_C6_FLIP: START queued EXEC dependency -> nonblocking implicit-fence flip',
              'NOUVEAU_C6_SELECT: PASS real inventory, paired roles, overrides and unavailable-device fallback hardware=1',
              'NOUVEAU_C6_FLIP: PASS pending RMFB/handle/fd teardown, isolated event queue and fresh-client recovery',
              'NOUVEAU_C6_FLIP: END six verified flips and pending teardown status=pass']+
             [f'NOUVEAU_C6_FLIP: PASS event frame={frame} ' for frame in range(6)]+
             [f'NOUVEAU_C6_FLIP: PENDING frame={frame} ' for frame in range(7)])
    if any(marker not in log for marker in markers):
        raise RuntimeError('C6 pending-work, event progression or teardown gate incomplete')

def stage_failure_bundle(source, destination, *, missing=None, fixtures=False):
    """Create private negative fixtures; never modify the verified bundle."""
    destination.mkdir(parents=True,exist_ok=True)
    names=('gsp','bootloader','booter_load','booter_unload')
    for name in names:
        if name!=missing:
            shutil.copyfile(source/f'{name}-570.144.bin',destination/f'{name}-570.144.bin')
    if fixtures:
        for absent in names:
            for name in names:
                if name==absent:continue
                data=(source/f'{name}-570.144.bin').read_bytes()
                # CPU-loading fixtures only: 2 MiB exercises vmalloc; these
                # files are never parsed or submitted to a GPU.
                if name=='gsp':data=data[:2*1024*1024]
                (destination/f'{name}-c4-{absent}.bin').write_bytes(data)

def verify_failed_probe(log, component):
    request=f'nvidia/ad106/gsp/{component}-570.144.bin'
    if (f'firmware "{request}" unavailable' not in log or
        not re.search(r'\[NOUVEAU\] probe end 0000:00:03\.0 result=-2\b',log) or
        '[  OK  ] LinuxKPI: nouveau failed-probe cleanup ' not in log or
        'NOUVEAU_PROBE: expected failed probe exposes no Nouveau DRM card' not in log or
        '[drm] Initialized nouveau' in log):
        raise RuntimeError('missing-firmware probe did not demonstrate expected failure and clean resource teardown')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--gpu',help='explicit host display BDF, e.g. 0000:01:00.0')
    p.add_argument('--preflight',action='store_true',help='read-only host inventory; no VM or device writes')
    p.add_argument('--software',action='store_true',help='gate-off firmware readiness VM, no VFIO devices')
    p.add_argument('--vfio-only',action='store_true',help='assign the reviewed GPU pair with Nouveau probing disabled; hardware baseline only')
    p.add_argument('--run',action='store_true',help='run after preparation; hardware needs both functions already bound to VFIO')
    p.add_argument('--host-on-igpu',action='store_true',help='confirm host desktop and monitor have moved off NVIDIA')
    p.add_argument('--rom',type=Path,help='optional explicitly supplied matching NVIDIA ROM; never use the AMD default ROM')
    p.add_argument('--kernel',type=Path,default=ROOT/'build/nouveau-c4/kernel')
    p.add_argument('--iso',type=Path,default=ROOT/'avoryos-x86_64.iso')
    p.add_argument('--firmware-dir',type=Path,help='explicit verified private GSP bundle for an isolated candidate')
    p.add_argument('--remove-on-shutdown',action='store_true',help='diagnostic: unregister Nouveau before guest power-off')
    p.add_argument('--pcie-root-port',action='store_true',help='place the GPU pair behind a dedicated PCIe root port at guest 0000:01:00.0/1')
    p.add_argument('--gpu-only',action='store_true',help='diagnostic: omit GPU audio from the guest; both host functions must remain isolated and VFIO-owned')
    p.add_argument('--gsp-trace',action='store_true',help='capture full GSP RPC payloads for comparison without changing firmware')
    p.add_argument('--c5-memory',action='store_true',help='test GEM mappings, ownership and synchronous explicit VM operations; no command execution yet')
    p.add_argument('--c5-copy',action='store_true',help='test CE channels, NVIF objects, explicit EXEC, GPU-copy bytes and bounded syncobj fence waits')
    p.add_argument('--c6-inventory',action='store_true',help='query physical NVIDIA KMS resources, connectors, modes, EDID and plane formats; no modeset')
    p.add_argument('--c6-prime',action='store_true',help='test Nouveau GART export to native VirtIO import, CPU byte visibility and lifetime; no presentation yet')
    p.add_argument('--c6-present',action='store_true',help='present two CPU-filled NVIDIA-exported patterns through VirtIO and verify QMP screenshots pixel-for-pixel')
    p.add_argument('--c6-gpu-present',action='store_true',help='copy two patterns into a userspace-unmapped NVIDIA destination with CE EXEC, wait on timeline fences, and verify VirtIO scanout pixels')
    p.add_argument('--c6-flips',action='store_true',help='test six nonblocking event flips behind provably pending EXEC dependencies, captured updates and pending-event close/recovery')
    p.add_argument('--c7-zink',action='store_true',help='require exact NVK device identity, offscreen Zink shader pixels and context restart')
    p.add_argument('--c7-xorg',action='store_true',help='extend --c7-zink with identity-selected NVIDIA Xorg/GLX and captured PRIME output')
    p.add_argument('--failure-tests',action='store_true',help='run native upstream GSP missing-file unwind tests, without GPU access')
    p.add_argument('--missing-firmware',choices=('gsp','bootloader','booter_load','booter_unload'),help='negative hardware probe: deliberately omit one verified 570 file')
    p.add_argument('--output-dir',type=Path,default=ROOT/'build/nouveau-c4/probe')
    p.add_argument('--serial-log',type=Path,help='append live serial to this log; private per-run logs are retained')
    p.add_argument('--live-serial',action='store_true',help='also stream serial to the terminal')
    p.add_argument('--display',choices=('none','gtk'),default='none',help='show a QEMU window or keep the diagnostic headless')
    p.add_argument('--full-screen',choices=('on','off'),default='off')
    p.add_argument('--qemu-extra',default='',help='additional QEMU options, as with the Makefile QEMU_EXTRA override')
    p.add_argument('--readiness',type=Path,
                   help='passing gate-off readiness result required for this exact hardware candidate')
    p.add_argument('--cpus',type=int,default=8);p.add_argument('--timeout',type=int,default=180)
    a=p.parse_args()
    if a.c7_xorg and not a.c7_zink:
        p.error('--c7-xorg requires --c7-zink')
    guest_gpu='0000:01:00.0' if a.pcie_root_port else GUEST_GPU_BDF
    if a.readiness is None:a.readiness=ROOT/f'build/nouveau-c4/readiness-handoff-{a.cpus}cpu/result.json'
    if a.cpus<1 or a.timeout<1:p.error('CPU count and timeout must be positive')
    if sum((a.c5_memory, a.c5_copy, a.c6_inventory, a.c6_prime, a.c6_present, a.c6_gpu_present, a.c6_flips, a.c7_zink)) > 1:
        p.error('select one C5/C6 oracle per run')
    if (a.c5_memory or a.c5_copy or a.c6_inventory or a.c6_prime or a.c6_present or a.c6_gpu_present or a.c6_flips or a.c7_zink) and (a.vfio_only or a.failure_tests or a.missing_firmware):
        p.error('C5/C6 oracles are separate from probe-gate and failed-firmware experiments')
    if (a.c6_present or a.c6_gpu_present or a.c6_flips or a.c7_xorg) and '-qmp' in shlex.split(a.qemu_extra):
        p.error('--c6-present manages its own private QMP socket')
    if a.vfio_only and (a.software or a.remove_on_shutdown or a.failure_tests or a.missing_firmware):
        p.error('--vfio-only is a separate hardware baseline without Nouveau probe/removal')
    if a.failure_tests and not a.software:
        p.error('--failure-tests runs in the gate-off software VM')
    if a.missing_firmware and (a.software or a.remove_on_shutdown or a.failure_tests):
        p.error('--missing-firmware is a separate hardware failure experiment')
    if (a.failure_tests or a.missing_firmware) and not a.firmware_dir:
        p.error('failure experiments require an explicit private 570 bundle')
    if not a.software and not a.gpu:p.error('--gpu is required for hardware preparation/preflight')
    inventory=inspect(a.gpu) if not a.software else None
    if a.preflight:
        if inventory is None:p.error('--preflight needs a hardware --gpu')
        print(json.dumps(inventory,indent=2));return 0 if not inventory['blockers'] else 2
    if a.run and not a.software:
        if not a.host_on_igpu:raise RuntimeError('move host desktop/monitor to iGPU, then use --host-on-igpu')
        if inventory['blockers']:raise RuntimeError('; '.join(inventory['blockers']))
    if not a.kernel.is_file() or not a.iso.is_file():raise RuntimeError('candidate kernel and boot ISO must exist')
    symbols=subprocess.check_output(['nm',str(a.kernel)],text=True)
    if ' nouveau_drm_init\n' not in symbols:raise RuntimeError('candidate does not contain the linked Nouveau driver')
    for symbol in ('asc_vfs_kernel_size64', 'asc_emergency_poweroff'):
        if f' {symbol}\n' not in symbols:raise RuntimeError(f'candidate is missing C4 support: {symbol}')
    if any(',' in str(x.absolute()) for x in (a.output_dir,a.kernel,a.iso) + ((a.rom,) if a.rom else ())):
        raise RuntimeError('QEMU paths must not contain commas')
    rom_sha=verify_rom(a.rom) if a.rom else None
    fw=ROOT/'build/nouveau/firmware'
    if a.firmware_dir:
        fw=a.firmware_dir.absolute()
        bundle=verify_firmware(fw)
        if (a.failure_tests or a.missing_firmware) and bundle['gsp_interface']!='570.144':
            raise RuntimeError('failure experiments require the 570.144 candidate')
    else:
        stage=load('nouveau_fw','nouveau-firmware-stage.py')
        stage.stage(ROOT/'build/nouveau-c0/firmware',fw,ROOT/'docs/nouveau/c0-lock.json')
    boot=load('nouveau_runner','gpu-pipeline-boot.py');boot.OUT=a.output_dir.absolute()
    if boot.OUT.is_symlink():raise RuntimeError('output directory is a symlink')
    boot.OUT.mkdir(parents=True,exist_ok=True)
    with (boot.OUT/'prepare.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        archive_run(boot.OUT)
        oracle=boot.OUT/'probe.elf'
        defines=['-DNOUVEAU_EXPECT_PROBE_FAILURE=1'] if a.missing_firmware else []
        if a.vfio_only:defines.append('-DNOUVEAU_EXPECT_NO_PROBE=1')
        if a.c6_flips:defines.append('-DNOUVEAU_PENDING_FLIPS=1')
        oracle_kind,oracle_filename=next(((kind,filename) for enabled,kind,filename in (
            (a.c7_zink,'c7-xorg' if a.c7_xorg else 'c7-zink','test_nouveau_xorg.c' if a.c7_xorg else 'test_nouveau_zink.c'),
            (a.c6_flips,'c6-flips','test_nouveau_gpu_present.c'),
            (a.c6_gpu_present,'c6-gpu-present','test_nouveau_gpu_present.c'),
            (a.c6_present,'c6-present','test_nouveau_present.c'),
            (a.c6_prime,'c6-prime','test_nouveau_prime.c'),
            (a.c6_inventory,'c6-inventory','test_nouveau_display.c'),
            (a.c5_copy,'c5-copy','test_nouveau_copy.c'),
            (a.c5_memory,'c5-memory','test_nouveau_memory.c')) if enabled),('probe','test_nouveau_probe.c'))
        oracle_source=ROOT/'userland'/oracle_filename
        oracle_sources=[oracle_source]
        oracle_uapi={}
        includes=[]
        if a.c5_memory or a.c5_copy or a.c6_inventory or a.c6_prime or a.c6_present or a.c6_gpu_present or a.c6_flips:
            uapi=ROOT/'build/nouveau-618/linux/include/uapi/drm'
            includes=['-I',str(uapi)]
            oracle_uapi={name:hashlib.sha256((uapi/name).read_bytes()).hexdigest() for name in ('drm.h','nouveau_drm.h')}
            if a.c6_inventory:
                oracle_uapi['drm_mode.h']=hashlib.sha256((uapi/'drm_mode.h').read_bytes()).hexdigest()
        if a.c6_prime or a.c6_present or a.c6_gpu_present or a.c6_flips:
            oracle_sources.append(ROOT/'userland/test_nouveau_memory.c')
            includes+=['-iquote',str(uapi.parent/'linux')]
            for name in ('drm/drm_mode.h','linux/dma-buf.h'):
                oracle_uapi[name]=hashlib.sha256((uapi.parent/name).read_bytes()).hexdigest()
        if a.c6_present or a.c6_gpu_present or a.c6_flips:
            oracle_sources.extend((ROOT/'userland/test_nouveau_prime.c',ROOT/'scripts/nouveau-prime-capture.py'))
        if a.c6_gpu_present or a.c6_flips:
            oracle_sources.extend((ROOT/'userland/test_nouveau_present.c',ROOT/'userland/test_nouveau_copy.c'))
        if a.c5_copy or a.c6_gpu_present or a.c6_flips:
            if a.c5_copy:oracle_sources.append(ROOT/'userland/test_nouveau_memory.c')
            mesa=ROOT/'build/nouveau-c0/mesa-source/mesa-26.2.5/src/nouveau'
            includes+=['-I',str(mesa/'drm'),'-I',str(mesa/'headers'),'-I',str(mesa/'headers/nvidia/classes')]
            for name in ('drm/nvif/ioctl.h','headers/drf.h','headers/nvtypes.h','headers/nvidia/classes/cl906f.h','headers/nvidia/classes/clc7b5.h'):
                oracle_uapi['mesa/'+name]=hashlib.sha256((mesa/name).read_bytes()).hexdigest()
        if a.c6_flips:
            header=ROOT/'kernel/linuxkpi/include/uapi/kpi_dmabuf.h'
            includes+=['-iquote',str(header.parent)]
            oracle_uapi['avory/kpi_dmabuf.h']=hashlib.sha256(header.read_bytes()).hexdigest()
            oracle_sources.extend((ROOT/'userland/drm_identify.c',ROOT/'initrd/drm-pick.sh'))
        extra_files={}
        if a.c6_flips:
            identify=boot.OUT/'drm-identify'
            subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror','-I',str(uapi),
                            str(ROOT/'userland/drm_identify.c'),'-o',str(identify)],check=True)
            extra_files={'/bin/drm-identify':identify,'/bin/drm-pick.sh':ROOT/'initrd/drm-pick.sh'}
        c7 = None
        extra_runtime = ('/bin/sh','/usr/bin/awk','/usr/bin/grep') if a.c6_flips else ()
        if a.c7_zink:
            c7 = load('nouveau_c7', 'nouveau-c7.py')
            extra_files, oracle_uapi = c7.build(oracle, boot.TREE, a.c7_xorg)
            extra_runtime = c7.runtime_paths(boot.TREE, a.c7_xorg)
            oracle_sources.append(ROOT/'scripts/nouveau-c7.py')
            if a.c7_xorg:
                oracle_sources.extend(ROOT/path for path in ('userland/test_nouveau_zink.c',
                    'userland/drm_identify.c', 'config/xorg/10-nouveau.conf',
                    'config/xorg/10-native.conf', 'config/xorg/avory-xorg-profile.sh',
                    'config/xorg/44-nouveau-env.sh',
                    'config/xorg/avory-nouveau-xorg.sh', 'config/xorg/avory-prime-display.sh',
                    'scripts/nouveau-prime-capture.py', 'scripts/nouveau-c7-capture.py'))
        else:
            subprocess.run(['cc','-static','-O2','-Wall','-Wextra','-Werror',*defines,*includes,str(oracle_source),'-o',str(oracle)],check=True)
        opts='kpi_amdgpu=0 kpi_nouveau=0 kpi_nouveau_fw_test=1'
        if not a.software and not a.vfio_only:
            opts=f'kpi_amdgpu=0 kpi_nouveau=1 kpi_nouveau_fw_test=1 kpi_nouveau_bdf={guest_gpu} nouveau.modeset=1 nouveau.runpm=0 nouveau.config=NvGspRm=1 nouveau.gsp_kvm_passthrough=1 nouveau.debug=info,gsp=trace drm.debug=0x4'
            if a.firmware_dir:
                opts=opts.replace(' nouveau.gsp_kvm_passthrough=1','').replace('gsp=trace','gsp=debug')
            if a.remove_on_shutdown:
                opts+=' kpi_nouveau_remove_on_shutdown=1'
        if a.failure_tests:opts+=' kpi_nouveau_failure_tests=1'
        if a.gsp_trace:opts=opts.replace('gsp=debug','gsp=trace')
        if a.missing_firmware:
            opts=opts.replace('kpi_nouveau_fw_test=1','kpi_nouveau_fw_test=0')
            opts+=' kpi_nouveau_probe_cleanup_test=1'
        with tempfile.TemporaryDirectory(prefix='firmware-fixture-',dir=boot.OUT) as tmp:
            extra=fw/'lib/firmware'
            if a.failure_tests or a.missing_firmware:
                extra=Path(tmp)/'lib/firmware'
                stage_failure_bundle(fw/'lib/firmware/nvidia/ad106/gsp',extra/'nvidia/ad106/gsp',
                                     missing=a.missing_firmware,fixtures=a.failure_tests)
            boot.prepare(a.software,kernel=a.kernel,iso=a.iso,oracle=oracle,
                         cmdline=opts,firmware_extra=extra,
                         firmware_exclusive_prefix='nvidia/ad106/gsp' if a.firmware_dir else None,
                         extra_files=extra_files,extra_runtime=extra_runtime)
        record=dict(host=inventory,software=a.software,cpus=a.cpus,output_dir=str(boot.OUT),guest_gpu=guest_gpu,pcie_root_port=a.pcie_root_port,gpu_only=a.gpu_only,cmdline=opts,
                    oracle_kind=oracle_kind,
                    oracle_source_sha256=hashlib.sha256(b'\0'.join(path.read_bytes() for path in oracle_sources)).hexdigest(),oracle_uapi_sha256=oracle_uapi,
                    kernel=str(a.kernel.absolute()),kernel_sha256=hashlib.sha256(a.kernel.read_bytes()).hexdigest(),
                    firmware=json.loads((fw/'manifest.json').read_text()),rom_sha256=rom_sha,
                    serial_log=str(a.serial_log.absolute()) if a.serial_log else None,display=a.display,
                    failure_tests=a.failure_tests,missing_firmware=a.missing_firmware,vfio_only=a.vfio_only,gsp_trace=a.gsp_trace)
        (boot.OUT/'inputs.json').write_text(json.dumps(record,indent=2)+'\n')
        if not a.run:return 0
        # Recheck ownership after lengthy ISO preparation, immediately before VFIO opens.
        if not a.software:
            if not a.readiness.is_file():raise RuntimeError('run the gate-off software readiness VM first')
            ready=json.loads(a.readiness.read_text())
            if not readiness_matches(ready,record):
                raise RuntimeError('readiness result does not qualify this kernel/firmware pair')
            latest=inspect(a.gpu)
            if latest['blockers']:raise RuntimeError('; '.join(latest['blockers']))
        devices=guest_devices(None if a.software else a.gpu,root_port=a.pcie_root_port,rom=a.rom,gpu_only=a.gpu_only)
        qemu_extra=a.qemu_extra
        if a.pcie_root_port:qemu_extra='-global q35-pcihost.pci-hole64-size=64G '+qemu_extra
        capture=None
        if (a.c6_present or a.c6_gpu_present or a.c6_flips or a.c7_xorg) and not a.software:
            if a.c7_xorg:
                capture=load('nouveau_c7_capture','nouveau-c7-capture.py').FrameCapture(boot.OUT)
            else:
                capture=load('nouveau_capture','nouveau-prime-capture.py').FrameCapture(boot.OUT,gpu_producer=a.c6_gpu_present or a.c6_flips,frame_count=6 if a.c6_flips else 2,pending_flips=a.c6_flips)
            if capture.endpoint.exists():
                raise RuntimeError('presentation QMP socket already exists; use a new output directory')
            if len(str(capture.endpoint).encode())>100:
                raise RuntimeError('presentation output path is too long for a Unix socket')
            qemu_extra+=' -qmp '+shlex.quote(f'unix:{capture.endpoint},server=on,wait=off')
        args=SimpleNamespace(software=a.software,cpus=a.cpus,gpu=a.gpu,timeout=a.timeout,strict_boot_tests=True,edu=False,
                             serial_log=a.serial_log,live_serial=a.live_serial,display=a.display,qemu_extra=qemu_extra,
                             gtk_options=f'gtk,full-screen={a.full_screen},zoom-to-fit=off,grab-on-hover=on,window-close=off')
        args.observe_serial=capture
        result_path=boot.OUT/'result.json'
        result_path.write_text(json.dumps(dict(record,status='running'),indent=2)+'\n')
        try:
            label='nouveau-vfio-only' if a.vfio_only else 'nouveau-software' if a.software else 'nouveau-hardware'
            status=boot.execute(args,vfio_devices=devices,label=label)
            if a.vfio_only and status==0:
                verify_vfio_only((boot.OUT/(label+'-serial.log')).read_text(errors='replace'))
            if a.c5_memory and status==0:
                verify_c5_memory((boot.OUT/(label+'-serial.log')).read_text(errors='replace'),software=a.software)
            if a.c5_copy and status==0:
                verify_c5_copy((boot.OUT/(label+'-serial.log')).read_text(errors='replace'),software=a.software)
            if a.c6_inventory and status==0:
                verify_c6_inventory((boot.OUT/(label+'-serial.log')).read_text(errors='replace'),software=a.software)
            if a.c6_prime and status==0:
                verify_c6_prime((boot.OUT/(label+'-serial.log')).read_text(errors='replace'),software=a.software)
            if a.c6_present and status==0:
                verify_c6_present((boot.OUT/(label+'-serial.log')).read_text(errors='replace'),software=a.software)
                if capture:capture.finish()
            if a.c6_gpu_present and status==0:
                verify_c6_gpu_present((boot.OUT/(label+'-serial.log')).read_text(errors='replace'),software=a.software)
                if capture:capture.finish()
            if a.c6_flips and status==0:
                verify_c6_flips((boot.OUT/(label+'-serial.log')).read_text(errors='replace'),software=a.software)
                if capture:capture.finish()
            if a.c7_zink and status==0:
                (c7.verify_xorg if a.c7_xorg else c7.verify)((boot.OUT/(label+'-serial.log')).read_text(errors='replace'),software=a.software)
                if capture:capture.finish()
            if a.remove_on_shutdown and not a.software and status==0:
                verify_cleanup((boot.OUT/'nouveau-hardware-serial.log').read_text(errors='replace'))
            if a.missing_firmware and status==0:
                verify_failed_probe((boot.OUT/'nouveau-hardware-serial.log').read_text(errors='replace'),a.missing_firmware)
            if a.software:
                log=(boot.OUT/'nouveau-software-serial.log').read_text(errors='replace')
                for entry in record['firmware']['files']:
                    if f'[  OK  ] LinuxKPI: fw {entry["request"]}' not in log:
                        raise RuntimeError(f'guest did not verify firmware: {entry["request"]}')
                if a.failure_tests and '[  OK  ] LinuxKPI: nouveau four missing-firmware stages x8 release all buffers and VM mappings' not in log:
                    raise RuntimeError('native missing-firmware failure matrix did not run')
            result_path.write_text(json.dumps(dict(record,status='pass' if status==0 else 'fail',exit=status),indent=2)+'\n')
            return status
        except BaseException as error:
            result_path.write_text(json.dumps(dict(record,status='fail',error=str(error)),indent=2)+'\n')
            raise

if __name__=='__main__':
    try:raise SystemExit(main())
    except (OSError,RuntimeError,ValueError,subprocess.CalledProcessError) as e:raise SystemExit(str(e))
