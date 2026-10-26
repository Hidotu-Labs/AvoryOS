#!/usr/bin/env python3
"""Check Mesa EGL/GBM rendering in a private virgl VM using the desktop rootfs."""
import argparse
import os
import re
import shutil
import subprocess
from pathlib import Path
from boot_probe import run_probe, ROOT
from test_virgl_boot import INIT
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--desktop', action='store_true', help='also test Xorg glamor and Weston EGL clients')
parser.add_argument('--compat', action='store_true', help='check legacy GL chunk-buffer replacement, textures and fog pixels')
parser.add_argument('--mouse', action='store_true', help='test Xorg USB movement/clicks through QMP')
parser.add_argument('--pointer', choices=('usb-tablet', 'usb-mouse'), default='usb-tablet')
parser.add_argument('--gtk-display', help='isolated host X11 display for GTK/XTest input and screen capture')
parser.add_argument('--gtk-wayland-display', help='isolated host Weston socket with the virgl input test module')
parser.add_argument('--host-wayland-display', help='show SDL on this host Wayland socket; inject test input through QMP')
parser.add_argument('--full-rootfs', action='store_true', help='test a private reflink of disk.img with boot services and the SDDM greeter')
parser.add_argument('--stress', action='store_true', help='exercise repeated fullscreen/windowed swaps and continuous input for a minute')
parser.add_argument('--frontend', choices=('sdl', 'gtk'), default='sdl',
                    help='native host frontend for display input tests (matches make run by default)')
parser.add_argument('--benchmark', action='store_true', help='also measure uncapped glxgears in the full desktop image')
parser.add_argument('--scene-benchmark', action='store_true', help='use a 1024-mesh submission scene instead of glxgears')
args = parser.parse_args()
if args.compat and (args.desktop or args.mouse):
    parser.error('--compat runs directly on GBM; omit --desktop/--mouse')
if args.scene_benchmark and not args.benchmark:
    parser.error('--scene-benchmark requires --benchmark')
if args.benchmark and not (args.mouse and args.full_rootfs):
    parser.error('--benchmark requires --mouse --full-rootfs')
if (args.gtk_display or args.gtk_wayland_display) and not args.mouse:
    parser.error('--gtk-display requires --mouse')
if args.host_wayland_display and not args.mouse:
    parser.error('--host-wayland-display requires --mouse')
source=Path(os.environ.get('AVORY_TEST_ROOTFS', ROOT/'build/arch/rootfs'))
output=ROOT/('build/virgl-mouse-probe' if args.mouse else 'build/virgl-mesa-probe')
output.mkdir(parents=True, exist_ok=True)
binary=output/'egl-probe'
compiler=ROOT/'toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc'
subprocess.run([str(compiler), '--sysroot='+str(source), '-O2', '-march=x86-64',
                str(ROOT/'scripts/tests'/('virgl_compat_probe.c' if args.compat else 'virgl_mouse_probe.c' if args.mouse else 'virgl_egl_probe.c')), '-o', str(binary),
                '-lEGL', '-lGL' if args.compat else '-lGLESv2', '-lgbm', '-lX11', '-lXi', '-lwayland-client'], check=True)
if args.scene_benchmark:
    subprocess.run([str(compiler), '--sysroot='+str(source), '-O2',
                    str(ROOT/'scripts/tests/virgl_scene_bench.c'), '-o', str(output/'virgl-scene'),
                    '-lGL', '-lX11'], check=True)
def guest_file(path):
    candidate = source/path
    for _ in range(64):
        if not candidate.is_symlink():
            return candidate
        target = os.readlink(candidate)
        candidate = source/target.lstrip('/') if target.startswith('/') else candidate.parent/target
    raise RuntimeError('symlink loop in guest rootfs: '+str(path))

def setup(root):
    shutil.copy2(str(binary),root/'bin/virgl-egl')
    pending=['usr/lib/libEGL_mesa.so.0', 'usr/lib/dri/virtio_gpu_dri.so', 'usr/lib/gbm/dri_gbm.so']
    if args.desktop or args.mouse:
        pending += ['usr/bin/seatd', 'usr/bin/xkbcomp', 'usr/bin/bash',
                    'usr/bin/weston', 'usr/lib/Xorg', 'usr/lib/libGLX_mesa.so.0',
                    'usr/lib/weston/kiosk-shell.so', 'usr/lib/libweston-15/drm-backend.so',
                    'usr/lib/libweston-15/gl-renderer.so',
                    'usr/lib/xorg/modules/libglamoregl.so',
                    'usr/lib/xorg/modules/drivers/modesetting_drv.so',
                    'usr/lib/xorg/modules/extensions/libglx.so',
                    'usr/lib/xorg/modules/input/evdev_drv.so']
        shutil.copytree(guest_file('usr/share/X11/xkb'), root/'usr/share/X11/xkb', dirs_exist_ok=True)
        (root/'bin/sh').symlink_to('/usr/bin/bash')
        (root/'etc/X11').mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT/'config/xorg/10-virgl.conf', root/'etc/X11/xorg.conf')
    pending += ['usr/lib/'+name for name in re.findall(r'\(NEEDED\).*?\[(.*?)\]',subprocess.check_output(['readelf','-d',str(binary)],text=True))]
    seen=set()
    while pending:
        rel=pending.pop()
        if rel in seen:continue
        seen.add(rel)
        src=guest_file(rel)
        if not src.exists():raise RuntimeError(str(src))
        dst=root/rel;dst.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(src,dst)
        for name in re.findall(r'\(NEEDED\).*?\[(.*?)\]',subprocess.check_output(['readelf','-d',str(src)],text=True)):
            candidates = [str(Path(rel).parent/name), 'usr/lib/'+name, 'usr/lib/weston/'+name]
            pending.append(next((candidate for candidate in candidates if guest_file(candidate).exists()), candidates[1]))
    (root/'lib64').mkdir(exist_ok=True)
    shutil.copy2(source/'usr/lib/ld-linux-x86-64.so.2',root/'lib64/ld-linux-x86-64.so.2')
    d=root/'usr/share/glvnd/egl_vendor.d';d.mkdir(parents=True,exist_ok=True)
    shutil.copy2(source/'usr/share/glvnd/egl_vendor.d/50_mesa.json',d/'50_mesa.json')
init=INIT.replace('VIRGL_BOOT:', 'VIRGL_MOUSE_BOOT:' if args.mouse else 'VIRGL_MESA_BOOT:').replace('#include <unistd.h>','#include <unistd.h>\n#include <sys/wait.h>\n#include <stdlib.h>')
if not args.mouse:
    init=init.replace('done:', '''done:
     if(!failures) {
      pid_t child=fork();if(child==0){setenv("EGL_LOG_LEVEL","debug",1);setenv("LIBGL_DEBUG","verbose",1);execl("/bin/virgl-egl","virgl-egl",NULL);_exit(127);}
      int status=0;if(child<0||waitpid(child,&status,0)<0||!WIFEXITED(status)||WEXITSTATUS(status))failures++;
     }
    ''',1)
if args.desktop or args.mouse:
    init = init.replace('#include <stdlib.h>', '#include <stdlib.h>\n#include <sys/stat.h>\n#include <signal.h>')
    desktop = (ROOT/'scripts/tests/virgl_desktop_probe.inc').read_text()
    if args.mouse:
        desktop = desktop.replace('mode<2', 'mode<1')
    if args.stress:
        desktop = desktop.replace('pid_t client=fork();if(!client){', 'pid_t client=fork();if(!client){setenv("VIRGL_STRESS","1",1);')
    if args.benchmark:
        desktop = desktop.replace('waitpid(client,&status,0);', '''waitpid(client,&status,0);
   FILE *perf=fopen("/tmp/perf.log","w");fprintf(perf,"VIRGL_PERF: before\\n");
   FILE *stats=fopen("/proc/drmstats","r");if(stats){char line[256];while(fgets(line,sizeof(line),stats))fputs(line,perf);fclose(stats);}
   pid_t gears=fork();if(!gears){
    setenv("vblank_mode","0",1);
    int log=open("/tmp/gears.log",O_CREAT|O_WRONLY|O_TRUNC,0644);dup2(log,1);dup2(log,2);close(log);
    execl("/usr/bin/stdbuf","stdbuf","-oL","/usr/bin/glxgears","-info","-geometry","800x600",NULL);_exit(127);
   }
   sleep(12);kill(gears,SIGTERM);waitpid(gears,NULL,0);
   FILE *gears_log=fopen("/tmp/gears.log","r");if(gears_log){char line[1024];while(fgets(line,sizeof(line),gears_log))fputs(line,perf);fclose(gears_log);}
   fprintf(perf,"VIRGL_PERF: after\\n");
   stats=fopen("/proc/drmstats","r");if(stats){char line[256];while(fgets(line,sizeof(line),stats))fputs(line,perf);fclose(stats);}fclose(perf);
''')
        desktop = desktop.replace('printf("DESKTOP:', '''FILE *perf_log=fopen("/tmp/perf.log","r");if(perf_log){char line[1024];while(fgets(line,sizeof(line),perf_log))fputs(line,stdout);fclose(perf_log);}
  printf("DESKTOP:''')
        if args.scene_benchmark:
            desktop = desktop.replace('"/usr/bin/glxgears","-info","-geometry","800x600"',
                                      '"/bin/virgl-scene"')
    if args.full_rootfs:
        desktop = desktop.replace('"-config","/etc/X11/xorg.conf"', '"-configdir","/etc/X11/xorg.conf.d"')
        desktop = desktop.replace('int status=-1;', '''
  pid_t greeter=fork();if(!greeter){setenv("QT_QPA_PLATFORM","xcb",1);int log=open("/tmp/greeter.log",O_CREAT|O_WRONLY|O_TRUNC,0644);dup2(log,1);dup2(log,2);close(log);execl("/usr/bin/sddm-greeter-qt6","sddm-greeter-qt6","--test-mode","--theme","/usr/share/sddm/themes/breeze",NULL);_exit(127);}sleep(3);
  int greeter_status=0;int greeter_alive=greeter>0&&waitpid(greeter,&greeter_status,WNOHANG)==0;printf("DESKTOP: SDDM greeter alive=%d status=%d\\n",greeter_alive,greeter_status);if(!greeter_alive)failures++;
  int status=-1;''')
        desktop = desktop.replace('kill(daemon,SIGTERM);', 'kill(greeter,SIGTERM);waitpid(greeter,NULL,0);kill(daemon,SIGTERM);')
        desktop = '''pid_t services=fork();if(!services){execl("/bin/sh","sh","/etc/init.d/rcS",NULL);_exit(127);}waitpid(services,NULL,0);\n'''+desktop
    init = init.replace('done:', 'done:\nif (!failures) {\n'+desktop+'\n}\n', 1)
qemu_args = []
started = None
host_result = {}
prepare_disk = None
if args.full_rootfs:
    if not args.mouse:
        parser.error('--full-rootfs requires --mouse')
    def prepare_disk(disk, out):
        subprocess.run(['cp', '--reflink=always', str(ROOT/'disk.img'), str(disk)], check=True)
        image = str(disk)+'?offset=1048576'
        commands = []
        if args.scene_benchmark:
            commands += ['rm /bin/virgl-scene', f'write {output}/virgl-scene /bin/virgl-scene',
                         'set_inode_field /bin/virgl-scene mode 0100755']
        for src, dst in [(out/'root/bin/probe-init','/bin/probe-init'), (binary,'/bin/virgl-egl'),
                         (ROOT/'config/xorg/10-virgl.conf','/usr/share/avory-xorg/10-modesetting-virgl.conf'),
                         (ROOT/'config/xorg/10-virgl.conf','/etc/X11/xorg.conf.d/10-modesetting.conf')]:
            commands += [f'rm {dst}', f'write {src} {dst}', f'set_inode_field {dst} mode 0100755']
        subprocess.run(['debugfs','-w','-f','/dev/stdin',image], input='\n'.join(commands)+'\n', text=True, check=True, stdout=subprocess.DEVNULL)
if args.mouse:
    import json
    import threading
    import time
    host_env = dict(os.environ, DISPLAY=args.gtk_display or os.environ.get('DISPLAY', ''), GDK_BACKEND='x11')
    if args.gtk_wayland_display:
        host_env.update(GDK_BACKEND='wayland', WAYLAND_DISPLAY=args.gtk_wayland_display)
    if args.host_wayland_display:
        host_env.update(SDL_VIDEODRIVER='wayland', WAYLAND_DISPLAY=args.host_wayland_display)
    host_input = output/'gtk-input'
    if args.gtk_display:
        subprocess.run(['cc', '-O2', str(ROOT/'scripts/tests/virgl_gtk_input.c'),
                        '-o', str(host_input), '-lX11', '-lXtst'], check=True)

    def started(process, out):
        def inject():
            def command(name, arguments):
                process.stdin.write((json.dumps({'execute': name, 'arguments': arguments})+'\n').encode())
                process.stdin.flush()
                while True:
                    line = process.stdout.readline()
                    if not line:
                        raise EOFError('QMP disconnected')
                    reply = json.loads(line)
                    if 'error' in reply:
                        raise RuntimeError(reply['error'])
                    if 'return' in reply:
                        return reply['return']
            try:
                process.stdout.readline()  # QMP greeting
                command('qmp_capabilities', {})
                deadline = time.monotonic()+100
                while 'MOUSE: INPUT READY' not in (out/'serial.log').read_text(errors='replace'):
                    if process.poll() is not None or time.monotonic()>deadline:
                        raise RuntimeError('mouse probe did not become ready')
                    time.sleep(.1)
                captured = False
                for index in range(240):
                    if process.poll() is not None:
                        break
                    if args.gtk_wayland_display:
                        Path(os.environ['AVORY_VIRGL_TEST_INPUT']).write_text(str(index))
                        if not captured and 'MOUSE: CAPTURE LIVE' in (out/'serial.log').read_text(errors='replace'):
                            previous = set(out.glob('wayland-screenshot-*.png'))
                            subprocess.run(['weston-screenshooter'], env=host_env, cwd=out, check=True)
                            files = set(out.glob('wayland-screenshot-*.png')) - previous
                            if len(files) != 1:
                                raise RuntimeError('host compositor did not save a live screenshot')
                            host_result['capture'] = files.pop()
                            captured = True
                        time.sleep(.35)
                        continue
                    if args.gtk_display and not captured and 'MOUSE: CAPTURE LIVE' in (out/'serial.log').read_text(errors='replace'):
                        subprocess.run([str(host_input), 'capture', str(out/'display.ppm')], env=host_env, check=True)
                        host_result['capture'] = out/'display.ppm'
                        captured = True
                    if args.gtk_display:
                        subprocess.run([str(host_input), str(index)], env=host_env, check=True)
                        time.sleep(.35)
                        continue
                    absolute = args.pointer == 'usb-tablet'
                    x = (8192 if index % 2 else 24576) if absolute else (32 if index % 2 else -32)
                    y = 16384 if absolute else 8
                    kind = 'abs' if absolute else 'rel'
                    command('input-send-event', {'events': [
                        {'type': kind, 'data': {'axis': 'x', 'value': x}},
                        {'type': kind, 'data': {'axis': 'y', 'value': y}}]})
                    if index < 30:
                        time.sleep(.35)
                        continue
                    command('input-send-event', {'events': [
                        {'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
                    time.sleep(.15)
                    command('input-send-event', {'events': [
                        {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
                    time.sleep(.35)
            except (BrokenPipeError, EOFError):
                pass  # Normal guest shutdown closes QMP.
            except Exception as error:
                host_result['error'] = error
        threading.Thread(target=inject, daemon=True).start()
    qemu_args = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0',
                 '-device', args.pointer+',bus=xhci.0']
    if os.environ.get('AVORY_TEST_INPUT_TRACE'):
        qemu_args += ['-trace', 'enable=gd_*',
                      '-trace', 'enable=input_event_*,file='+str(output/'input-trace.log')]
host_display = args.frontend+',gl=on,show-cursor=off'
if args.frontend == 'gtk':
    host_display += ',zoom-to-fit=off,grab-on-hover=on'
result = run_probe('virgl-mouse-boot' if args.mouse else 'virgl-mesa-boot', init,
          'VIRGL_MOUSE_BOOT: PASS' if args.mouse else 'VIRGL_MESA_BOOT: PASS', setup=setup,
          gpu='virtio-vga-gl,xres=1280,yres=800', display=host_display if args.gtk_display or args.gtk_wayland_display or args.host_wayland_display else 'sdl,gl=on', disk_mib=512, timeout=180,
          cpus=8, memory='4G',
          accel=os.environ.get('AVORY_TEST_ACCEL', 'kvm'),
          qemu_env=host_env if args.gtk_display or args.gtk_wayland_display or args.host_wayland_display else dict(os.environ, SDL_VIDEODRIVER='offscreen'),
          qemu_args=qemu_args, started=started, qmp_stdio=args.mouse, prepare_disk=prepare_disk)
if args.mouse and 'MOUSE: END' not in (result/'serial.log').read_text(errors='replace'):
    raise RuntimeError('mouse executable did not report its assertions')
if args.benchmark:
    log = (result/'serial.log').read_text(errors='replace')
    samples = [float(value) for value in re.findall(r'frames in [\d.]+ seconds = ([\d.]+) FPS', log)]
    if len(samples) < 2 or min(samples) <= 0 or not re.search(r'GL_RENDERER\s*= virgl', log):
        raise RuntimeError('glxgears did not complete two accelerated benchmark samples')
    print(f'VIRGL_PERF: mean_fps={sum(samples)/len(samples):.3f} samples={len(samples)}')
if 'error' in host_result:
    raise host_result['error']
if args.gtk_display or args.gtk_wayland_display:
    from PIL import Image
    if 'capture' not in host_result:
        raise RuntimeError('no live host frame was captured')
    with Image.open(host_result['capture']) as screen:
        pixels = screen.convert('RGB')
        colors = pixels.getcolors(pixels.width*pixels.height)
        counts = {color: count for count,color in colors}
        if counts.get((255,0,0),0)<30000 or counts.get((0,0,255),0)<30000 or counts.get((0,255,0),0)<3000:
            raise RuntimeError('host scanout did not contain the complete RGB animation pattern')
    print('VIRGL_MOUSE_BOOT: live host RGB scanout PASS:', host_result['capture'])
