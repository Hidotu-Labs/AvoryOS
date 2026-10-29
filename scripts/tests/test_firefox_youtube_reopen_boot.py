#!/usr/bin/env python3
"""Verify YouTube tab cycling in KDE Plasma 6 with RTL8139 and Marionette."""
import argparse
import base64
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import threading
import time
from boot_probe import ROOT, run_probe
from test_firefox_responsiveness_boot import INIT, prepare_desktop_disk, assert_cpu_recovery

class Marionette:
    def __init__(self, port):
        deadline=time.monotonic()+100
        while True:
            try:
                self.socket=socket.create_connection(('127.0.0.1',port),timeout=5)
                self.socket.settimeout(100)
                print("MARIONETTE_GREETING: "+str(self.read()),flush=True)
                break
            except (OSError,EOFError):
                if time.monotonic()>deadline: raise
                time.sleep(.5)
        self.sequence=0
    def recv(self,n):
        data=b''
        while len(data)<n:
            part=self.socket.recv(n-len(data))
            if not part: raise EOFError('Browser automation disconnected')
            data+=part
        return data
    def read(self):
        prefix=b''
        while True:
            ch=self.recv(1)
            if ch==b':':break
            prefix+=ch
            if len(prefix)>12:raise RuntimeError('Invalid Marionette frame')
        return json.loads(self.recv(int(prefix)))
    def call(self, name, parameters=None):
        print("MARIONETTE_COMMAND: "+name,flush=True)
        self.sequence+=1
        body=json.dumps([0,self.sequence,name,parameters or {}]).encode()
        self.socket.sendall(str(len(body)).encode()+b':'+body)
        reply=self.read()
        if reply[1]!=self.sequence:raise RuntimeError('Unexpected Marionette response')
        if reply[2]:raise RuntimeError(str(reply[2]))
        result=reply[3]
        return result.get('value',result) if isinstance(result,dict) else result


def verify_result(out, cycles, soak_video=False, max_reopen_seconds=30):
    """Validate retained VM evidence without having to rerun the browser."""
    serial=(out/'serial.log').read_text(errors='replace')
    marker=out.name.upper().replace('-','_')+': PASS'
    if marker not in serial:
        raise RuntimeError('The native VM monitor did not complete successfully')
    for failure in ('[PMM] CORRUPTION:','[ USER FAULT ]','HANG REPORT'):
        if failure in serial:
            raise RuntimeError(f'Kernel failure during browser sequence: {failure}; see {out}/serial.log')
    if '[RTL8139] receive and interrupt paths enabled' not in serial:
        raise RuntimeError('The RTL8139 receive path did not start')

    # The desktop image omits pacman's package database. Read the installed
    # application's QString version literal without launching another Qt app
    # (and activating additional desktop services) during the reproduction.
    binary=out/'plasmashell.elf'
    binary.unlink(missing_ok=True)
    subprocess.run(['debugfs','-R',f'dump /usr/bin/plasmashell {binary}',out/'root.img'],capture_output=True,check=True)
    strings=subprocess.check_output(['strings','-a','-el',binary],text=True)
    versions=re.findall(r'^6\.\d+\.\d+$',strings,re.MULTILINE)
    if not versions:
        raise RuntimeError('KDE Plasma 6 version confirmation is missing')
    (out/'plasma-version.log').write_text('plasmashell '+', '.join(versions)+' (installed ELF version literal)\n')

    state=json.loads((out/'browser-events.json').read_text())
    if not state['complete']:
        raise RuntimeError(f'YouTube cycle did not complete: {state.get("error")}')
    events={e.get('label'):e for e in state['events']}
    for label in ['first-video-played-15s']+[f'reopened-video-{i}-played-15s' for i in range(1,cycles+1)]:
        video=events.get(label,{}).get('data',{}).get('video')
        if not video or video['time']<10 or video['frames']<10 or video['paused']:
            raise RuntimeError(f'Missing advancing playback: {label}')
    loads=[e for e in state['events'] if e.get('label')=='reopened-home-loaded']
    if len(loads)!=cycles or any(e['load_seconds']>max_reopen_seconds for e in loads):
        raise RuntimeError('The required reopen cycles did not meet the loading deadline')
    if not soak_video:
        if 'final-playing-tab-closed' not in events:
            raise RuntimeError('The playing tab was not closed for CPU recovery')
        assert_cpu_recovery((out/'monitor.log').read_text(errors='replace'))
    else:
        last=events.get('after-reopen-5',{}).get('data',{})
        video=last.get('video')
        if last.get('playerError') or not video or video['paused'] or video['time']<60:
            raise RuntimeError('Reopened video did not sustain the additional playback window')
    print('PASS: verified KDE Plasma 6 / RTL8139 YouTube tab close/reopen and playback',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url',default='https://www.youtube.com/watch?v=aqz-KE-bpKQ')
    parser.add_argument('--new-url',default='https://www.youtube.com/')
    parser.add_argument('--name',default='firefox-youtube-reopen-driver')
    parser.add_argument('--cycles',type=int,default=3)
    parser.add_argument('--http-log',action='store_true')
    parser.add_argument('--soak-video',action='store_true')
    parser.add_argument('--max-reopen-seconds',type=float,default=30)
    parser.add_argument('--verify-existing',type=Path)
    parser.add_argument('--plasma',action='store_true',default=True,help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.cycles<1:
        parser.error('--cycles must be positive')
    if args.verify_existing:
        verify_result(args.verify_existing.resolve(),args.cycles,args.soak_video,args.max_reopen_seconds)
        return
    work=ROOT/'build'/args.name
    work.mkdir(exist_ok=True)
    state={'events':[],'complete':False}
    driver_start=time.monotonic()
    def event(data):
        data['seconds']=round(time.monotonic()-driver_start,3)
        state['events'].append(data)
        (work/'browser-events.json').write_text(json.dumps(state,indent=2))
        print('BROWSER_STATE: '+json.dumps(data),flush=True)
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1',0));port=reserved.getsockname()[1]
    session=work/'session.sh';client=work/'client.sh'
    session.write_text('''#!/bin/bash
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export XDG_RUNTIME_DIR=/tmp/runtime
export XDG_SESSION_TYPE=wayland
export KWIN_DRM_DEVICES=/dev/dri/card0
export MOZ_ENABLE_WAYLAND=1
unset DISPLAY MOZ_HEADLESS
/etc/init.d/rcS
export PULSE_SERVER=unix:/run/pipewire/pulse/native
export XDG_CURRENT_DESKTOP=KDE
exec /usr/bin/avory-plasma-wayland
''')
    client.write_text('''#!/bin/bash
exec /usr/lib/firefox/firefox --marionette --no-remote --profile /tmp/profile about:blank
''')
    if args.http_log:
        client.write_text(client.read_text().replace('exec /usr/lib/firefox/firefox','export MOZ_LOG=timestamp,nsHttp:4\nexec /usr/lib/firefox/firefox'))
    autostart=work/'autostart.desktop'
    autostart.write_text('[Desktop Entry]\nType=Application\nName=Firefox reproduction probe\nExec=/root/reopen-client.sh\nOnlyShowIn=KDE;\n')
    session.chmod(0o755);client.chmod(0o755)
    # This guest has no IPv4 localhost route. For the test only, expose the
    # automation listener to QEMU user networking, whose host forward remains
    # restricted to 127.0.0.1. The production Firefox archive is untouched.
    archive=work/'automation-omni.ja'
    if not archive.exists():
        tree=work/'omni-tree';tree.mkdir(exist_ok=True)
        unpack=subprocess.run(['unzip','-oq',ROOT/'build/arch/rootfs/usr/lib/firefox/omni.ja','-d',tree],capture_output=True)
        names=subprocess.run(['unzip','-Z1',ROOT/'build/arch/rootfs/usr/lib/firefox/omni.ja'],capture_output=True,text=True).stdout.splitlines()
        if not names or any(not (tree/name).is_file() for name in names):raise RuntimeError(unpack.stderr.decode())
        server=tree/'chrome/remote/content/marionette/server.sys.mjs'
        source=server.read_text()
        if 'const flags = KeepWhenOffline | LoopbackOnly;' not in source:raise RuntimeError('Unexpected Firefox automation server')
        server.write_text(source.replace('const flags = KeepWhenOffline | LoopbackOnly;','const flags = KeepWhenOffline;'))
        import zipfile
        with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_STORED) as output:
            for path in tree.rglob('*'):
                if path.is_file():output.write(path,path.relative_to(tree).as_posix())
    def prepare(disk,out):
        prepare_desktop_disk(disk,out,[(session,'/root/reopen-session.sh'),(client,'/root/reopen-client.sh'),(autostart,'/etc/xdg/autostart/avory-firefox-reopen-probe.desktop')])
        subprocess.run(['debugfs','-w','-R','rm /usr/lib/firefox/omni.ja',disk],capture_output=True,check=True)
        result=subprocess.run(['debugfs','-w','-R',f'write {archive} /usr/lib/firefox/omni.ja',disk],capture_output=True,text=True,check=True)
        if 'Allocated inode:' not in result.stdout:raise RuntimeError(result.stderr)
    def started(process,out):
        def drive():
            try:
                browser=Marionette(port)
                capabilities=browser.call('WebDriver:NewSession',{'pageLoadStrategy':'none'})
                event({'label':'session-capabilities','data':capabilities})
                if capabilities['capabilities']['pageLoadStrategy']!='none':
                    raise RuntimeError('Marionette did not apply the requested navigation strategy')
                browser.call('WebDriver:SetTimeouts',{'pageLoad':40000,'script':10000,'implicit':0})
                def record(label):
                    data=browser.call('WebDriver:ExecuteScript',{'script':'''let v=document.querySelector('video'),p=document.getElementById('movie_player');return {url:location.href,title:document.title,ready:document.readyState,searchReady:!!document.querySelector('input[name=search_query]'),text:document.body?.innerText.slice(0,1000),playerError:document.querySelector('.ytp-error-content-wrap')?.innerText||null,video:v?{time:v.currentTime,ready:v.readyState,paused:v.paused,width:v.videoWidth,height:v.videoHeight,quality:p?.getPlaybackQuality?.(),frames:v.getVideoPlaybackQuality().totalVideoFrames,error:v.error?{code:v.error.code,message:v.error.message}:null}:null};''','args':[],'newSandbox':True})
                    # Navigation with strategy=none can replace the content
                    # global while a script is queued; Marionette then returns
                    # null. Treat it as a loading sample and poll again.
                    data=data or {}
                    event({'label':label,'data':data})
                    return data
                def navigate(url=None):
                    try:browser.call('WebDriver:Navigate',{'url':url or args.url})
                    except RuntimeError as error:
                        event({'navigation_error':str(error),'url':url or args.url})
                def shot(label):
                    encoded=browser.call('WebDriver:TakeScreenshot',{'full':False,'scroll':False})
                    (out/(label+'.png')).write_bytes(base64.b64decode(encoded))
                def wait_page(label):
                    deadline=time.monotonic()+60
                    while time.monotonic()<deadline:
                        data=record(label+'-loading')
                        # A fresh profile may receive an empty recommendation
                        # feed. A complete page with YouTube's working search
                        # input is ready regardless of feed text length.
                        if data.get('title') and data.get('ready')=='complete' and (data.get('searchReady') or len(data.get('text') or '')>100):
                            return data
                        time.sleep(2)
                    raise RuntimeError(label+' did not finish loading in 60 seconds')
                def play(label):
                    deadline=time.monotonic()+60
                    while time.monotonic()<deadline:
                        data=record(label+'-waiting')
                        video=data.get('video') if data.get('url','').startswith(args.url) else None
                        if video:
                            browser.call('WebDriver:ExecuteScript',{'script':"let p=document.getElementById('movie_player');p?.setPlaybackQualityRange?.('hd1080','hd1080');p?.setPlaybackQuality?.('hd1080');return true;",'args':[]})
                            if not video['paused'] and video['ready']>=2 and video['frames']>0:
                                before=video
                                time.sleep(15)
                                after=record(label+'-played-15s')['video']
                                if after['time']-before['time']<10 or after['frames']<=before['frames']:
                                    raise RuntimeError(label+' did not sustain playback for 10 seconds')
                                return after
                            if video['paused']:
                                try:
                                    element=browser.call('WebDriver:FindElement',{'using':'css selector','value':'.ytp-play-button'})
                                    identifier=element.get('element-6066-11e4-a52e-4f735466cecf',element.get('ELEMENT'))
                                    browser.call('WebDriver:ElementClick',{'id':identifier})
                                except RuntimeError as error:
                                    event({'label':label+'-play-click-error','error':str(error)})
                        time.sleep(2)
                    raise RuntimeError(label+' did not start playback in 60 seconds')
                first=browser.call('WebDriver:GetWindowHandle')
                navigate('https://www.youtube.com/');wait_page('initial-home')
                navigate();play('first-video');shot('first-page')
                # Explicitly verify and switch real browser tabs, rather than
                # relying on the currently focused QEMU input console.
                for cycle in range(1,args.cycles+1):
                    second=browser.call('WebDriver:NewWindow',{'type':'tab','focus':True})['handle']
                    browser.call('WebDriver:SwitchToWindow',{'handle':second})
                    browser.call('WebDriver:SwitchToWindow',{'handle':first})
                    browser.call('WebDriver:CloseWindow')
                    browser.call('WebDriver:SwitchToWindow',{'handle':second})
                    event({'label':'original-tab-closed','cycle':cycle,'handles':browser.call('WebDriver:GetWindowHandles')})
                    # Open a new tab, close the playing YouTube tab, and reopen
                    # YouTube in the remaining tab, matching the reported sequence.
                    navigation_started=time.monotonic()
                    navigate(args.new_url)
                    wait_page(f'reopened-home-{cycle}')
                    load_seconds=time.monotonic()-navigation_started
                    event({'label':'reopened-home-loaded','cycle':cycle,'load_seconds':round(load_seconds,3),'url':args.new_url})
                    if load_seconds>args.max_reopen_seconds:
                        raise RuntimeError(f'YouTube reopen took {load_seconds:.1f}s > {args.max_reopen_seconds}s')
                    shot(f'reopened-home-{cycle}')
                    navigate();play(f'reopened-video-{cycle}')
                    shot(f'reopened-video-{cycle}')
                    first=second
                if not args.soak_video:
                    blank=browser.call('WebDriver:NewWindow',{'type':'tab','focus':True})['handle']
                    browser.call('WebDriver:SwitchToWindow',{'handle':first})
                    browser.call('WebDriver:CloseWindow')
                    browser.call('WebDriver:SwitchToWindow',{'handle':blank})
                    event({'label':'final-playing-tab-closed'})
                for i in range(6):time.sleep(10);record('after-reopen-'+str(i))
                shot('after-reopen')
                last=state['events'][-1]['data']
                if args.soak_video and (last.get('playerError') or not last.get('video') or last['video']['paused'] or last['video']['time']<60):
                    raise RuntimeError('Reopened video did not sustain the additional playback window')
                state['complete']=True
                browser.socket.close()
            except Exception as error:
                state['error']=repr(error);print('BROWSER_DRIVER_ERROR: '+repr(error),flush=True)
            finally:(out/'browser-events.json').write_text(json.dumps(state,indent=2))
        threading.Thread(target=drive,daemon=True).start()
    init=INIT.replace('FIREFOX_RESPONSIVENESS_BOOT',args.name.upper().replace('-','_'))
    init=init.replace('puts("'+args.name.upper().replace('-','_')+': START");','puts("'+args.name.upper().replace('-','_')+': START");int output=open("/root/reopen-monitor.log",O_CREAT|O_TRUNC|O_WRONLY,0600);dup2(output,1);dup2(output,2);close(output);')
    init=init.replace('if(!browser){if(setsid()', 'if(!browser){int output=open("/root/reopen-browser.log",O_CREAT|O_TRUNC|O_WRONLY|O_APPEND,0600);dup2(output,1);dup2(output,2);close(output);if(setsid()')
    init=init.replace('"/usr/lib/firefox/firefox","firefox","--headless","--no-remote","--profile","/tmp/profile",','"/usr/bin/bash","bash","/root/reopen-session.sh",')
    init=init.replace('setenv("MOZ_HEADLESS","1",1);','')
    init=init.replace('setenv("MOZ_DISABLE_CONTENT_SANDBOX","1",1);','')
    init=init.replace(r'user_pref(\"security.sandbox.content.level\",0);\n','')
    assert 'MOZ_DISABLE_CONTENT_SANDBOX' not in init and 'security.sandbox.content.level' not in init
    init=init.replace('if(window==2)tasks(browser);','if(window%3==0)dump("/proc/sysdiag");')
    duration=360 if args.http_log or args.soak_video else 300
    init=init.replace('180000',str(duration*1000)).replace('window>=17',f'window>={duration//10-1}')
    init=init.replace('dump("/proc/tlbstats");','dump("/proc/tlbstats");dump("/proc/stat");dump("/proc/net/dev");dump("/proc/net/tcp");')
    marker=args.name.upper().replace('-','_')
    init=init.replace('printf("'+marker+': %s windows=', 'int kmsg=open("/dev/kmsg",O_WRONLY);if(kmsg>=0){dprintf(kmsg,"'+marker+': %s\\n",completed?"PASS":"FAIL");close(kmsg);}printf("'+marker+': %s windows=')
    env=dict(os.environ,SDL_VIDEODRIVER='offscreen',LD_LIBRARY_PATH=str(ROOT/'build/virgl-host/lib')+':'+os.environ.get('LD_LIBRARY_PATH',''))
    out=work
    try:
        out=run_probe(args.name,init,marker+': PASS',cpus=8,memory='4G',timeout=duration+90,prepare_disk=prepare,started=started,
            gpu='virtio-vga-gl',display='sdl,gl=on',qemu_env=env,kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init',
            qemu_args=['-qmp',f'unix:{work}/qmp.sock,server=on,wait=off','-chardev',f'socket,path={work}/gdb.sock,server=on,wait=off,id=debug','-gdb','chardev:debug','-netdev',f'user,id=net0,hostfwd=tcp:127.0.0.1:{port}-:2828','-device','rtl8139,netdev=net0','-object',f'filter-dump,id=capture,netdev=net0,file={work}/network.pcap','-audiodev','none,id=snd0','-device','intel-hda','-device','hda-duplex,audiodev=snd0'])
    finally:
        if (out/'root.img').exists():
            for guest,name in [('/root/reopen-monitor.log','monitor.log'),('/root/reopen-browser.log','browser.log'),('/root/reopen-plasma-version','plasma-version.log')]:
                (out/name).unlink(missing_ok=True)
                subprocess.run(['debugfs','-R',f'dump {guest} {out/name}',out/'root.img'],capture_output=True,check=False)
    verify_result(out,args.cycles,args.soak_video,args.max_reopen_seconds)

if __name__=='__main__':main()
