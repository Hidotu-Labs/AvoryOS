#!/usr/bin/env python3
"""Exercise launch prevention with fake sysfs and clocks; never open VFIO."""
from pathlib import Path
import importlib.util
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('resume_check', ROOT/'scripts/vfio_resume_check.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix='vfio-resume-test-') as tmp:
    sysroot = Path(tmp)
    dev = sysroot/'bus/pci/devices/0000:0e:00.0'
    dev.mkdir(parents=True)
    (dev/'vendor').write_text('0x1002\n')
    (dev/'device').write_text('0x164e\n')
    success = sysroot/'power/suspend_stats/success'
    success.parent.mkdir(parents=True)
    success.write_text('0\n')
    command = ['qemu-system-x86_64','-device','vfio-pci,host=0e:00.0,addr=03.0']
    def clocks(gap):
        return lambda ident: 200.0 + (gap if ident==time.CLOCK_BOOTTIME else 0)
    def rejected(cmd=command, clock=clocks(0)):
        try:
            module.check_raphael_resume(cmd,sysroot,clock)
        except RuntimeError:
            return
        raise AssertionError('unsafe launch accepted')
    data=module.check_raphael_resume(command,sysroot,clocks(0))
    assert data['successful_suspends']==0 and data['raphael_devices']==['0000:0e:00.0']
    # Both full-domain and short-domain arguments must receive the same policy.
    full=['qemu','-device','vfio-pci,host=0000:0e:00.0']
    success.write_text('1\n');rejected();rejected(full)
    trial=module.check_raphael_resume(command,sysroot,clocks(20),allow_resumed=True)
    assert trial['resumed_host_trial'] and trial['successful_suspends']==1
    # Clock accounting catches suspend when sysfs is unreadable or stale.
    success.write_text('0\n');rejected(clock=clocks(500))
    success.unlink();rejected(clock=clocks(500))
    assert module.check_raphael_resume(command,sysroot,clocks(0))['successful_suspends'] is None
    def no_clock(ident):
        raise OSError('clock unavailable')
    rejected(clock=no_clock)
    success.write_text('0\n')
    assert module.check_raphael_resume(command,sysroot,no_clock)['successful_suspends']==0
    # Other GPUs, software-only guests and unrelated processes are unaffected.
    success.write_text('8\n')
    (dev/'device').write_text('0x9999\n')
    assert module.check_raphael_resume(command,sysroot,clocks(500))=={'raphael_devices':[]}
    assert module.check_raphael_resume(['qemu','-device','virtio-vga'],sysroot,no_clock)=={'raphael_devices':[]}
    assert module.check_raphael_resume(['true'],sysroot,no_clock)=={'raphael_devices':[]}
    (dev/'vendor').unlink();rejected()
print('PASS: cold boot allowed; resumed Raphael blocked; fallback/unknown history and device scoping checked')
