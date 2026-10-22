#!/usr/bin/env python3
"""Exercise the power-policy helper against real temporary sysfs/proc trees."""
from pathlib import Path
import importlib.util
import json
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nvpower', ROOT/'scripts/nouveau-host-power.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def fixture(base):
    sys = base/'sys'
    proc = base/'proc'
    (proc/'sys/kernel/random').mkdir(parents=True)
    (proc/'sys/kernel/random/boot_id').write_text('test-boot-1\n')
    (proc/'uptime').write_text('12.0 10.0\n')
    (proc/'cpuinfo').write_text('flags : clflush sse2\n')
    pci = sys/'bus/pci/devices'
    pci.mkdir(parents=True)
    for driver in ('vfio-pci', 'amdgpu', 'pcieport'):
        (sys/'bus/pci/drivers'/driver).mkdir(parents=True)
    bridge = sys/'devices/pci0000:00'/m.BRIDGE
    bridge.mkdir(parents=True)
    group = sys/'kernel/iommu_groups/12'
    (group/'devices').mkdir(parents=True)
    for bdf, vendor, device, cls, driver in (
        (m.BRIDGE, 0x1022, 0x14da, '0x060400', 'pcieport'),
        (m.GPU, 0x10de, 0x2803, '0x030000', 'vfio-pci'),
        (m.AUDIO, 0x10de, 0x22bd, '0x040300', 'vfio-pci'),
    ):
        dev = bridge if bdf == m.BRIDGE else bridge/bdf
        dev.mkdir(exist_ok=True)
        (pci/bdf).symlink_to(dev)
        for attr, value in {'vendor': hex(vendor), 'device': hex(device), 'class': cls,
                            'boot_vga': '0', 'power_state': 'D0', 'reset_method': 'flr bus',
                            'd3cold_allowed': '1', 'current_link_width': '8'}.items():
            (dev/attr).write_text(value+'\n')
        (dev/'power').mkdir()
        (dev/'power/control').write_text('auto\n')
        (dev/'power/runtime_status').write_text('active\n')
        (dev/'reset').write_text('never write reset\n')
        config = bytearray(64)
        config[:4] = (device << 16 | vendor).to_bytes(4, 'little')
        config[61] = 1 if bdf == m.GPU else 2
        (dev/'config').write_bytes(config)
        (dev/'driver').symlink_to(sys/'bus/pci/drivers'/driver)
        if bdf != m.BRIDGE:
            (dev/'iommu_group').symlink_to(group)
            (group/'devices'/bdf).symlink_to(dev)
    igpu = sys/'devices/pci0000:00/0000:0e:00.0'
    igpu.mkdir()
    (pci/igpu.name).symlink_to(igpu)
    (igpu/'vendor').write_text('0x1002')
    (igpu/'device').write_text('0x164e')
    (igpu/'driver').symlink_to(sys/'bus/pci/drivers/amdgpu')
    drm = sys/'class/drm/card0'
    drm.mkdir(parents=True)
    (drm/'device').symlink_to(igpu)
    mod = sys/'module/vfio_pci/parameters'
    mod.mkdir(parents=True)
    (mod/'disable_idle_d3').write_text('Y\n')
    return sys, proc, pci


def contents(root):
    return {str(p.relative_to(root)): p.read_bytes() for p in root.rglob('*')
            if p.is_file() and not p.is_symlink()}


def rejects(callback):
    try:
        callback()
    except (OSError, RuntimeError):
        return
    raise AssertionError('unsafe operation accepted')


with tempfile.TemporaryDirectory(prefix='nvpower-') as tmp:
    base = Path(tmp)
    sys, proc, pci = fixture(base)
    state = base/'state.json'
    inspect = lambda: m.inspect(sys, proc)
    initial = contents(sys)
    assert inspect()['healthy_for_policy'] and not inspect()['policy_pinned']
    assert contents(sys) == initial  # Inventory is read-only.
    writes = []
    def writer(path, value):
        writes.append(str(path))
        m.write_attr(path, value)
    m.apply(inspect(), state, sys, writer)
    assert len(writes) == 6 and inspect()['policy_pinned']
    assert all(p.endswith(('/power/control', '/d3cold_allowed')) for p in writes)
    assert state.stat().st_mode & 0o077 == 0
    pinned = contents(sys)
    rejects(lambda: m.apply(inspect(), state, sys))
    assert contents(sys) == pinned
    m.restore(inspect(), state, sys)
    assert contents(sys) == initial and not state.exists()

    # A mid-apply write failure restores every attempted attribute.
    counter = [0]
    def fail_third(path, value):
        counter[0] += 1
        if counter[0] == 3:
            raise OSError('injected sysfs failure')
        m.write_attr(path, value)
    rejects(lambda: m.apply(inspect(), state, sys, fail_third))
    assert contents(sys) == initial and not state.exists()
    m.apply(inspect(), state, sys)
    counter[0] = 0
    rejects(lambda: m.restore(inspect(), state, sys, fail_third))
    assert contents(sys) == pinned and state.exists()
    # State and policy tampering cannot redirect or overwrite host attributes.
    saved = state.read_bytes()
    bad = json.loads(saved)
    bad['boot_id'] = 'old-boot'
    state.write_text(json.dumps(bad))
    rejects(lambda: m.restore(inspect(), state, sys))
    bad['boot_id'] = 'test-boot-1'
    bad['original']['../../outside'] = {}
    state.write_text(json.dumps(bad))
    rejects(lambda: m.restore(inspect(), state, sys))
    state.write_bytes(saved)
    (pci/m.GPU/'power/control').write_text('auto\n')
    rejects(lambda: m.restore(inspect(), state, sys))
    (pci/m.GPU/'power/control').write_text('on\n')
    m.restore(inspect(), state, sys)
    assert contents(sys) == initial
    state.symlink_to(base/'outside')
    rejects(lambda: m.apply(inspect(), state, sys))
    state.unlink()

    def unsafe_case(path, bad_value):
        previous = path.read_bytes()
        path.write_bytes(bad_value)
        assert not inspect()['healthy_for_policy']
        before = contents(sys)
        rejects(lambda: m.apply(inspect(), state, sys))
        assert not state.exists() and contents(sys) == before
        path.write_bytes(previous)
    unsafe_case(pci/m.GPU/'config', b'\xff'*64)
    unsafe_case(pci/m.BRIDGE/'config', b'\xff'*64)
    unsafe_case(pci/m.GPU/'power_state', b'D3cold\n')
    unsafe_case(pci/m.GPU/'boot_vga', b'1\n')
    unsafe_case(sys/'module/vfio_pci/parameters/disable_idle_d3', b'N\n')
    process = proc/'42'
    (process/'fd').mkdir(parents=True)
    (process/'cmdline').write_bytes(b'/bin/tool\0')
    (process/'fd/3').symlink_to('/dev/vfio/12')
    assert not inspect()['healthy_for_policy']
    rejects(lambda: m.apply(inspect(), state, sys))
    (process/'fd/3').unlink()
    extra = (pci/m.BRIDGE).resolve()/'0000:02:00.0'
    extra.mkdir()
    (pci/extra.name).symlink_to(extra)
    assert not inspect()['healthy_for_policy']
    rejects(lambda: m.apply(inspect(), state, sys))
    assert not state.exists()

print('PASS: dedicated topology/identity/ownership/power guards, six policy writes only, read-only status, apply/restore, rollback, stale/tampered state and symlink refusal')
