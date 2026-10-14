#!/usr/bin/env python3
"""One explicitly approved GTK X-close/reopen trial. Default: prepare only.

--run starts exactly two physical guests: first initializes, receives a GTK
WM_DELETE_WINDOW close request; second initializes and powers off normally.
No retries, reset helpers, host setting changes or forced process termination.
If a guest remains running, leave QEMU and its isolated X server alive.
"""
import argparse, importlib.util, json, os, signal
from pathlib import Path
import select, subprocess, sys, tempfile, time
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('psp_probe',ROOT/'scripts/amdgpu-psp-boot.py')
probe=importlib.util.module_from_spec(spec);spec.loader.exec_module(probe)
boot=probe.boot
HOLD='XCLOSE_TEST: initialization complete; holding 45 seconds'
INIT=r'''
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);
 puts("XCLOSE_TEST: initialization complete; holding 45 seconds");
 sleep(45);
 puts("XCLOSE_TEST: requesting orderly poweroff");
 reboot(RB_POWER_OFF);
 perror("XCLOSE_TEST poweroff");
 for(;;)sleep(60);
}
'''

def no_existing_gpu_vm():
 for p in Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:
   if not (p/'exe').resolve(strict=True).name.startswith('qemu-system-'):continue
   cmd=(p/'cmdline').read_bytes()
  except OSError:continue
  if b'host=0e:00.0' in cmd or b'host=0000:0e:00.0' in cmd:
   raise RuntimeError('Existing Raphael QEMU process: '+p.name)

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--run',action='store_true')
 parser.add_argument('--psp-reinit',action='store_true',help='Enable scoped PSP stop-before-create candidate')
 parser.add_argument('--mode2',action='store_true',help='Enable scoped Raphael MODE2 init reset candidate')
 parser.add_argument('--driver-defaults',action='store_true',help='Omit recovery parameters and validate native defaults')
 parser.add_argument('--close-mode',choices=('window','sigint'),default='window')
 args=parser.parse_args()
 if args.driver_defaults and (args.mode2 or args.psp_reinit):
  parser.error('--driver-defaults cannot be combined with recovery overrides')
 out=Path(tempfile.mkdtemp(prefix='trial-',dir=ROOT/'build/amdgpu-xclose'))
 boot.OUT=out;boot.INIT=INIT
 recovery='' if args.driver_defaults else f'kpi_amdgpu_mode2_reset={int(args.mode2)} kpi_amdgpu_psp_reinit={int(args.psp_reinit)} '
 cmdline=('kpi_amdgpu=1 kpi_prime_offload=1 '+recovery+
          'amdgpu.runpm=0 amdgpu.ppfeaturemask=0xfff73fff drm.debug=0x4 init=/bin/probe-init')
 disk,iso=boot.prepare(probe.probe.stage_firmware,cmdline)
 helper=out/'window-close'
 subprocess.run(['cc','-Wall','-Wextra','-Werror',ROOT/'scripts/tests/qemu_window_close.c','-lX11','-o',helper],check=True)
 physical=['qemu-system-x86_64','-M','q35','-cpu','host','-enable-kvm','-smp','8','-m','4G',
 '-drive',f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
 '-cdrom',str(iso),'-boot','d','-drive',f'file={disk},format=raw,if=none,id=probe',
 '-device','nvme,serial=xclose-probe,drive=probe','-vga','none',
 '-device','virtio-vga,addr=06.0,xres=1280,yres=800','-device','edu,addr=07.0',
 '-display','gtk,window-close=on','-monitor','none','-no-reboot',
 '-device',f'vfio-pci,host=0e:00.0,addr=03.0,rombar=1,romfile={ROOT}/build/vfio/vbios.rom']
 (out/'prepared-command.json').write_text(json.dumps(physical,indent=2)+'\n')
 print('Trial directory:',out,flush=True)
 if not args.run:
  print('Preparation only; no physical guest launched.');return 0
 no_existing_gpu_vm()
 dev=Path('/sys/bus/pci/devices/0000:0e:00.0')
 assert (dev/'vendor').read_text().strip()=='0x1002' and (dev/'device').read_text().strip()=='0x164e'
 assert (dev/'driver').resolve().name=='vfio-pci'
 host_boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip()
 state={'phase':'prepared','host_boot_id':host_boot,'physical_launches':0,'mode2':args.mode2,
        'psp_reinit':args.psp_reinit,'close_mode':args.close_mode}
 if args.driver_defaults:
  state.update(mode2='driver-default',psp_reinit='driver-default')
 def save(phase):
  state['phase']=phase
  with (out/'state.json').open('w') as f:
   json.dump(state,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
 def same_host():
  assert Path('/proc/sys/kernel/random/boot_id').read_text().strip()==host_boot,'Host reboot; no continuation authorized'
 server=None;process=None
 log=(out/'controller.log').open('ab',buffering=0)
 try:
  r,w=os.pipe()
  server=subprocess.Popen([ROOT/'build/tools/xvfb/usr/bin/Xvfb','-displayfd',str(w),'-screen','0','1280x800x24','-nolisten','tcp','-ac'],pass_fds=(w,),stdout=log,stderr=log,start_new_session=True)
  os.close(w)
  assert select.select([r],[],[],10)[0],'Xvfb startup timeout'
  display=os.read(r,32).decode().strip();os.close(r)
  assert display.isdigit();env=dict(os.environ,DISPLAY=':'+display,GDK_BACKEND='x11')
  state['xvfb_pid']=server.pid;state['display']=':'+display
  # Validate GTK's actual close handler without any passed device first.
  control=subprocess.Popen(['qemu-system-x86_64','-M','q35','-m','128M','-S','-name','xclose-control',
    '-display','gtk,window-close=on','-device','virtio-vga','-monitor','none','-serial','none'],env=env,stdout=log,stderr=log,start_new_session=True)
  time.sleep(2)
  subprocess.run([helper,'xclose-control'],env=env,stdout=log,stderr=log,check=True)
  assert control.wait(timeout=10)==0
  save('gpu-free-gtk-close-verified');print('PASS: GPU-free GTK WM_DELETE_WINDOW close',flush=True)
  for number in (1,2):
   same_host();no_existing_gpu_vm()
   serial=out/f'boot{number}-serial.log'
   command=physical+['-name',f'raphael-xclose-{number}','-chardev',
     f'socket,id=trial-log,path={out}/serial{number}.sock,server=on,wait=off,logfile={serial},logappend=on','-serial','chardev:trial-log']
   launcher=[sys.executable,ROOT/'scripts/qemu-prime-log.py','--output-root',out/'launch-evidence','--serial-log',serial,'--',*command]
   state['physical_launches']=number;save(f'boot{number}-starting')
   process=subprocess.Popen([str(x) for x in launcher],env=env,stdout=log,stderr=log,start_new_session=True)
   state['launcher_pid']=process.pid;save(f'boot{number}-running')
   deadline=time.monotonic()+120
   while process.poll() is None and time.monotonic()<deadline:
    text=serial.read_text(errors='replace') if serial.exists() else ''
    if number==1 and HOLD in text:
     if 'Initialized amdgpu ' not in text or 'Fatal error during GPU init' in text:
      save('first-init-failed-no-xclose-or-reopen');break
     assert 'XCLOSE_TEST: requesting orderly poweroff' not in text
     save('first-ready-sending-gtk-close')
     if args.close_mode=='window':
      subprocess.run([helper,'raphael-xclose-1'],env=env,stdout=log,stderr=log,check=True)
     else:
      process.send_signal(signal.SIGINT)
     print('First GPU initialized; abrupt close sent via '+args.close_mode,flush=True)
     assert process.wait(timeout=15)==0
     text=serial.read_text(errors='replace')
     assert 'shutting down pci amdgpu' not in text and 'System power-off requested' not in text
     save('first-xclosed');break
    time.sleep(.2)
   if number==1 and state['phase']!='first-xclosed':
    print('First initialization did not reach approved close boundary; no reopen.',flush=True)
    return 1
   if number==2:
    if process.poll() is None:
     save('second-still-running-no-force-close');print('Second guest remains alive; no forced close or retry.',flush=True);return 2
    text=serial.read_text(errors='replace');same_host()
    ok=(process.returncode==0 and 'Initialized amdgpu ' in text and 'Fatal error during GPU init' not in text
        and 'THREAD FAULT:' not in text and 'pci amdgpu 0000:00:03.0 shutdown complete' in text)
    state['second_initialized']=ok;state['second_returncode']=process.returncode
    save('two-boot-sequence-complete')
    print('Second initialization:', 'PASS' if ok else 'FAIL',flush=True)
    return 0 if ok else 1
 finally:
  if server is not None and (process is None or process.poll() is not None):
   server.terminate();server.wait(timeout=5)
  elif server is not None:
   print('Leaving QEMU and Xvfb alive for orderly guest shutdown.',flush=True)
  log.close()

if __name__=='__main__':
 (ROOT/'build/amdgpu-xclose').mkdir(exist_ok=True)
 sys.exit(main())
