#!/usr/bin/env python3
"""Graphical Firefox VP9/Opus playback in a disposable desktop filesystem."""
import argparse
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlsplit

from boot_probe import ROOT, run_probe
from test_firefox_responsiveness_boot import INIT, prepare_desktop_disk, assert_cpu_recovery


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audio', action='store_true')
    parser.add_argument('--virgl', action='store_true')
    parser.add_argument('--clip', type=Path, default=ROOT/'build/firefox-video/clip.webm')
    parser.add_argument('--mse', action='store_true', help='append/evict video through MediaSource like streaming sites')
    parser.add_argument('--memory', default='4G')
    parser.add_argument('--name', default='firefox-video-boot')
    parser.add_argument('--wayland', action='store_true', help='use KWin Wayland instead of IceWM/Xorg')
    parser.add_argument('--close-page', action='store_true', help='destroy the video document after playback, then measure idle recovery')
    parser.add_argument('--cpus', type=int, default=4)
    parser.add_argument('--codec', choices=('vp9', 'av1'), default='vp9')
    args = parser.parse_args()
    work = ROOT/'build/firefox-video'
    work.mkdir(exist_ok=True)
    clip = args.clip
    if not clip.exists():
        raise RuntimeError('Generate build/firefox-video/clip.webm with ffmpeg first')
    state = {'samples': [], 'complete': False, 'closed': False}
    started = time.monotonic()
    page = ('''<!doctype html><style>body{margin:0;background:#171b26;color:white;font:24px sans-serif}video{width:100vw;height:90vh}</style>
<video id=v autoplay loop %s src="/clip.webm"></video><div id=status>Video test</div><script>
let start=performance.now(),pending=false,callbacks=0,last=0,totalTime=0;
function frame(){callbacks++;v.requestVideoFrameCallback(frame)}
if(v.requestVideoFrameCallback)v.requestVideoFrameCallback(frame);
v.play().catch(e=>fetch('/error?message='+encodeURIComponent(e)));
setInterval(()=>{let delta=v.currentTime-last;if(delta>0&&delta<2)totalTime+=delta;last=v.currentTime;
let q=v.getVideoPlaybackQuality();let data={time:v.currentTime,played:totalTime,frames:q.totalVideoFrames,dropped:q.droppedVideoFrames,callbacks,ready:v.readyState,paused:v.paused,wall:(performance.now()-start)/1000};
status.textContent=JSON.stringify(data);if(!pending){pending=true;fetch('/sample?data='+encodeURIComponent(JSON.stringify(data))).finally(()=>pending=false)}
if(data.wall>60&&data.played>30)fetch('/complete');},1000);</script>''' % ('' if args.audio else 'muted')).encode()
    media = clip.read_bytes()
    if args.mse:
        page = page.replace(b'src="/clip.webm"', b'')
        page = page.replace(b'v.play().catch(e=>fetch', b'''let ms=new MediaSource();v.src=URL.createObjectURL(ms);
ms.addEventListener('sourceopen',async()=>{try{
let data=await (await fetch('/clip.webm')).arrayBuffer();
let sb=ms.addSourceBuffer('video/webm; codecs="vp9,opus"');sb.mode='sequence';
function feed(){if(sb.updating)return;let end=sb.buffered.length?sb.buffered.end(sb.buffered.length-1):0;
if(v.currentTime>20&&sb.buffered.length&&sb.buffered.start(0)<v.currentTime-15){sb.remove(0,v.currentTime-10);return;}
if(end-v.currentTime<12)sb.appendBuffer(data);}
sb.addEventListener('updateend',feed);setInterval(feed,500);feed();
}catch(e){fetch('/error?message='+encodeURIComponent(e))}});
v.play().catch(e=>fetch''')
    if args.close_page:
        page = page.replace(b"if(data.wall>60&&data.played>30)fetch('/complete');",
                            b"if(data.wall>35&&data.played>30)fetch('/complete').then(()=>location.replace('/closed'));")
    page = page.replace(b'codecs="vp9,opus"', ('codecs="'+args.codec+',opus"').encode())

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def do_GET(self):
            path = urlsplit(self.path)
            if path.path == '/sample':
                sample = json.loads(parse_qs(path.query)['data'][0])
                state['samples'].append(sample)
                print(f'VIDEO: host_s={time.monotonic()-started:.1f} {sample}', flush=True)
            if path.path == '/error':
                print('VIDEO ERROR: '+self.path, flush=True)
            if path.path == '/complete':
                state['complete'] = True
            if path.path == '/closed':
                state['closed'] = True
                print('VIDEO: document destroyed; beginning idle recovery', flush=True)
            body = page if path.path == '/' else media if path.path == '/clip.webm' else b'ok'
            begin, end = 0, len(body)-1
            ranged = path.path == '/clip.webm' and 'Range' in self.headers
            if ranged:
                match = re.fullmatch(r'bytes=(\d+)-(\d*)', self.headers['Range'])
                if not match:
                    self.send_error(416)
                    return
                begin = int(match[1])
                end = min(int(match[2]) if match[2] else end, end)
                if begin > end:
                    self.send_error(416)
                    return
            self.send_response(206 if ranged else 200)
            self.send_header('Content-Type', 'video/webm' if path.path == '/clip.webm' else 'text/html')
            self.send_header('Content-Length', str(end-begin+1))
            if ranged:
                self.send_header('Content-Range', f'bytes {begin}-{end}/{len(body)}')
            self.send_header('Accept-Ranges', 'bytes')
            self.end_headers()
            self.wfile.write(body[begin:end+1])

        def log_message(self, *unused):
            pass

    session = work/'session.sh'
    session.write_text('''#!/bin/bash
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export DISPLAY=:0
export XDG_RUNTIME_DIR=/tmp/runtime
unset MOZ_HEADLESS
mkdir -p /tmp/xorg-empty
''' + ('''/etc/init.d/rcS
export PULSE_SERVER=unix:/run/pipewire/pulse/native
''' if args.audio else '') + '''
/usr/lib/Xorg :0 vt1 -config /root/video-xorg.conf -configdir /tmp/xorg-empty -logfile /tmp/xorg.log -noreset -nolisten tcp > /tmp/xorg-console.log 2>&1 &
for i in {1..40}; do [ -S /tmp/.X11-unix/X0 ] && break; sleep 1; done
if [ ! -S /tmp/.X11-unix/X0 ]; then cat /tmp/xorg.log /tmp/xorg-console.log; exit 1; fi
icewm > /tmp/icewm.log 2>&1 &
exec dbus-run-session -- /usr/lib/firefox/firefox --no-remote --profile /tmp/profile "$1"
''')
    session.chmod(0o755)
    client = work/'client.sh'
    client.write_text('''#!/bin/bash
export MOZ_ENABLE_WAYLAND=1
exec /usr/lib/firefox/firefox --no-remote --profile /tmp/profile "$AVORY_VIDEO_URL"
''')
    client.chmod(0o755)
    if args.wayland:
        content = session.read_text()
        content = content[:content.index('/usr/lib/Xorg')] + '''
unset DISPLAY
export XDG_SESSION_TYPE=wayland
export KWIN_DRM_DEVICES=/dev/dri/card0
export AVORY_VIDEO_URL="$1"
exec dbus-run-session -- /usr/bin/kwin_wayland --no-lockscreen --exit-with-session /root/video-client.sh
'''
        session.write_text(content)
    config = ROOT/('config/xorg/10-virgl.conf' if args.virgl else 'config/xorg/10-native.conf')

    def prepare(disk, out):
        prepare_desktop_disk(disk, out, [(session, '/root/video-session.sh'),
                                        (config, '/root/video-xorg.conf'),
                                        (client, '/root/video-client.sh')])

    def capture(process, out):
        def command(name, arguments=None):
            process.stdin.write((json.dumps({'execute': name, 'arguments': arguments or {}})+'\n').encode())
            process.stdin.flush()
            while True:
                reply = json.loads(process.stdout.readline())
                if 'error' in reply:
                    raise RuntimeError(reply['error'])
                if 'return' in reply:
                    return reply['return']

        def screenshots():
            try:
                json.loads(process.stdout.readline())
                command('qmp_capabilities')
                for number in (1, 2):
                    time.sleep(35)
                    command('screendump', {'filename': str(out/f'video-{number}.ppm')})
            except Exception as error:
                print('SCREENSHOT: '+str(error), flush=True)

        threading.Thread(target=screenshots, daemon=True).start()

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    init = INIT.replace('FIREFOX_RESPONSIVENESS_BOOT', 'FIREFOX_VIDEO_BOOT')
    # KD_GRAPHICS suppresses ordinary console writes. Retain monitor output
    # in the private filesystem and emit only the final marker via kmsg.
    init = init.replace('puts("FIREFOX_VIDEO_BOOT: START");',
                        'puts("FIREFOX_VIDEO_BOOT: START");int output=open("/root/video-monitor.log",O_CREAT|O_TRUNC|O_WRONLY,0600);if(output>=0){dup2(output,1);dup2(output,2);close(output);}')
    init = init.replace('if(!browser){if(setsid()',
                        'if(!browser){int output=open("/tmp/browser.log",O_CREAT|O_TRUNC|O_WRONLY,0600);if(output>=0){dup2(output,1);dup2(output,2);close(output);}if(setsid()')
    init = init.replace('printf("FIREFOX_VIDEO_BOOT: %s windows=',
                        'int kmsg=open("/dev/kmsg",O_WRONLY);if(kmsg>=0){dprintf(kmsg,"FIREFOX_VIDEO_BOOT: %s\\n",completed?"PASS":"FAIL");close(kmsg);}printf("FIREFOX_VIDEO_BOOT: %s windows=')
    init = init.replace('setenv("MOZ_HEADLESS","1",1);', '')
    init = init.replace('"/usr/lib/firefox/firefox","firefox","--headless","--no-remote","--profile","/tmp/profile",',
                        '"/usr/bin/bash","bash","/root/video-session.sh",')
    init = init.replace('security.sandbox.content.level\\\",0);',
                        'security.sandbox.content.level\\\",0);\\nuser_pref(\\\"media.autoplay.default\\\",0);')
    init = init.replace('180000', '120000').replace('window>=17', 'window>=11')
    init = init.replace('dump("/proc/tlbstats");', 'dump("/proc/tlbstats");dump("/proc/stat");dump("/proc/drmstats");')
    init = init.replace('dump("/proc/sysdiag");', 'dump("/proc/sysdiag");dump("/tmp/xorg.log");dump("/var/log/pipewire.log");dump("/tmp/browser.log");')
    qargs = ['-netdev', 'user,id=net0', '-device', 'rtl8139,netdev=net0',
             '-audiodev', 'none,id=snd0', '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0',
             '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd', '-device', 'usb-mouse']
    env = dict(os.environ, SDL_VIDEODRIVER='offscreen')
    if args.virgl:
        env['LD_LIBRARY_PATH'] = str(ROOT/'build/virgl-host/lib')+':'+env.get('LD_LIBRARY_PATH', '')
    marker = args.name.upper().replace('-', '_')
    init = init.replace('FIREFOX_VIDEO_BOOT', marker)
    try:
        out = run_probe(args.name, init.replace('__PORT__', str(server.server_port)),
                        marker+': PASS', cpus=args.cpus, memory=args.memory, timeout=240,
                        prepare_disk=prepare, qemu_args=qargs, qmp_stdio=True, started=capture,
                        gpu='virtio-vga-gl' if args.virgl else 'virtio-vga',
                        display='sdl,gl=on' if args.virgl else 'none', qemu_env=env,
                        kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init')
        (out/'video-metrics.json').write_text(json.dumps(state, indent=2))
        subprocess.run(['debugfs', '-R', f'dump /root/video-monitor.log {out}/monitor.log', out/'root.img'],
                       capture_output=True, check=True)
        log = (out/'monitor.log').read_text(errors='replace')
        delays = [int(value) for value in re.findall(r'worst_wake_ms=(\d+)', log)]
        print(f'VIDEO_MONITOR: wake_ms={delays}', flush=True)
        if not state['complete'] or not state['samples'] or max(s['frames'] for s in state['samples']) < 600:
            raise RuntimeError('Video did not complete 30 seconds/600 frames; inspect playback metrics')
        if args.close_page and not state['closed']:
            raise RuntimeError('Browser did not destroy the video document')
        if args.close_page:
            assert_cpu_recovery(log)
        print('PASS: graphical Firefox video playback completed', flush=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
