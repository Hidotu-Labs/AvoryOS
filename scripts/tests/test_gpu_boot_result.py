#!/usr/bin/env python3
"""A VM which has already exited must still fail on kernel faults/timeouts."""
from pathlib import Path
from types import SimpleNamespace
import importlib.util,tempfile,contextlib,io
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('boot',root/'scripts/gpu-pipeline-boot.py')
boot=importlib.util.module_from_spec(spec);spec.loader.exec_module(boot)
with tempfile.TemporaryDirectory(prefix='gpu-boot-result-') as tmp:
 boot.OUT=Path(tmp)
 cases=[('[KPF] THREAD FAULT',False),('[PANIC] panic',False),('LinuxKPI: boot self-tests timed out',True),('[ FAIL ] LinuxKPI: failed check',True),('',True)]
 for marker,strict in cases:
  class ExitedVM:
   def __init__(self,*args,**kwargs):
    (boot.OUT/'software-serial.log').write_text(marker+'\nGPU_PIPELINE_BOOT: START\nGPU_PIPELINE_BOOT: END exit=0 signal=0\n')
   def poll(self):return 0
  boot.subprocess.Popen=ExitedVM
  try:
   with contextlib.redirect_stdout(io.StringIO()):
    result=boot.execute(SimpleNamespace(software=True,cpus=1,gpu='',timeout=10,strict_boot_tests=strict,edu=False))
   assert not marker and result==0, 'false success: '+marker
  except RuntimeError:
   assert marker, 'clean exited guest failed'
print('PASS: completed VM kernel faults, strict timeout/failure and clean exit classification')

# Dedicated NVIDIA callers supply both exact devices and never inherit AMD ROMs.
with tempfile.TemporaryDirectory(prefix='nouveau-runner-') as tmp:
 boot.OUT=Path(tmp)
 captured=[]
 class ExitedNouveau:
  def __init__(self,command,**kwargs):
   captured.extend(command)
   (boot.OUT/'nouveau-hardware-serial.log').write_text('GPU_PIPELINE_BOOT: START\nGPU_PIPELINE_BOOT: END exit=0 signal=0\n')
  def poll(self):return 0
 boot.subprocess.Popen=ExitedNouveau
 devices=['vfio-pci,host=0000:01:00.0,addr=03.0,multifunction=on,rombar=1',
          'vfio-pci,host=0000:01:00.1,addr=03.1']
 with contextlib.redirect_stdout(io.StringIO()):
  result=boot.execute(SimpleNamespace(software=False,cpus=1,gpu='0000:01:00.0',timeout=10,strict_boot_tests=True,edu=False),vfio_devices=devices,label='nouveau-hardware')
 assert result==0
 assert [x for x in captured if x.startswith('vfio-pci,')]==devices
 assert not any('romfile=' in x or 'disk.img' in x for x in captured)
 assert any('snapshot=on' in x for x in captured)
print('PASS: dedicated NVIDIA runner preserves exact two-function assignment, isolated disk and no inherited AMD ROM')

# Public history must not affect current-run classification; final shutdown
# bytes and live output must survive a process that has already exited.
with tempfile.TemporaryDirectory(prefix='nouveau-public-serial-') as tmp:
 boot.OUT=Path(tmp)/'private';boot.OUT.mkdir()
 public=Path(tmp)/'build/logs/nouveau-c4.log'
 public.parent.mkdir(parents=True);public.write_text('[KPF] THREAD FAULT from an older run\n')
 captured=[]
 class ExitedWithTail:
  def __init__(self,command,**kwargs):
   self.finished=False;captured[:]=command
   (boot.OUT/'nouveau-hardware-serial.log').write_text('GPU_PIPELINE_BOOT: START\nGPU_PIPELINE_BOOT: END exit=0 signal=0\n')
  def poll(self):
   if not self.finished:
    with (boot.OUT/'nouveau-hardware-serial.log').open('a') as stream:stream.write('FINAL_SHUTDOWN_BYTES\n')
    self.finished=True
   return 0
 boot.subprocess.Popen=ExitedWithTail
 args=SimpleNamespace(software=False,cpus=1,gpu='0000:01:00.0',timeout=10,
                      strict_boot_tests=True,edu=False,serial_log=public,live_serial=True,display='gtk')
 for _ in range(2):
  terminal=io.StringIO()
  with contextlib.redirect_stdout(terminal):
   assert boot.execute(args,vfio_devices=devices,label='nouveau-hardware')==0
  assert terminal.getvalue().count('GPU_PIPELINE_BOOT: END')==1
  assert 'FINAL_SHUTDOWN_BYTES' in terminal.getvalue()
 assert captured[captured.index('-display')+1]=='gtk,zoom-to-fit=on,grab-on-hover=on,window-close=off'
 args.gtk_options='gtk,full-screen=off,zoom-to-fit=off,grab-on-hover=on,window-close=off'
 args.qemu_extra='-name "Nouveau C4"'
 with contextlib.redirect_stdout(io.StringIO()):assert boot.execute(args,vfio_devices=devices,label='nouveau-hardware')==0
 assert captured[captured.index('-display')+1]==args.gtk_options
 assert captured[captured.index('-name')+1]=='Nouveau C4'
 history=public.read_text();assert history.count('[GPU-BOOT]')==3 and history.count('FINAL_SHUTDOWN_BYTES')==3
 assert history.count('[KPF] THREAD FAULT from an older run')==1
 # Another launch must fail before opening QEMU when the public log is owned.
 with boot.SerialMirror(boot.OUT/'other.log',public):
  try:boot.SerialMirror(boot.OUT/'another.log',public)
  except RuntimeError as error:assert 'another launch owns' in str(error)
  else:raise AssertionError('concurrent public serial owner accepted')
 try:boot.SerialMirror(public,public)
 except RuntimeError:pass
 else:raise AssertionError('recursive serial mirror accepted')
print('PASS: append/live/final serial bytes, prior-fault isolation, GTK option and exclusive public-log ownership')

with tempfile.TemporaryDirectory(prefix='nouveau-qemu-error-') as tmp:
 boot.OUT=Path(tmp)
 class FailedQemu:
  def __init__(self,command,**kwargs):
   kwargs['stderr'].write(b'vfio 0000:01:00.0: invalid PCI interrupt pin 255\n');kwargs['stderr'].flush()
  def poll(self):return 1
 boot.subprocess.Popen=FailedQemu;terminal=io.StringIO()
 try:
  with contextlib.redirect_stdout(terminal):boot.execute(args,vfio_devices=devices,label='nouveau-hardware')
 except RuntimeError as error:assert 'cold boot' in str(error)
 else:raise AssertionError('QEMU startup failure accepted')
 assert 'invalid PCI interrupt pin 255' in terminal.getvalue()
print('PASS: preboot VFIO failure is visible in terminal with recovery guidance')

with tempfile.TemporaryDirectory(prefix='nouveau-d3-') as tmp:
 boot.OUT=Path(tmp)
 class BootedWithoutGpu:
  def __init__(self,command,**kwargs):
   kwargs['stderr'].write(b'vfio: Unable to power on device, stuck in D3\n');kwargs['stderr'].flush()
   (boot.OUT/'nouveau-hardware-serial.log').write_text('GPU_PIPELINE_BOOT: START\nGPU_PIPELINE_BOOT: END exit=1 signal=0\n')
  def poll(self):return 0
 boot.subprocess.Popen=BootedWithoutGpu;terminal=io.StringIO()
 with contextlib.redirect_stdout(terminal):assert boot.execute(args,vfio_devices=devices,label='nouveau-hardware')==1
 assert 'stuck in D3' in terminal.getvalue() and 'cold boot' in terminal.getvalue()
print('PASS: VFIO D3 errors stay visible even when the guest boots and the oracle fails')
