#!/usr/bin/env python3
"""Measure CPU recovery after real Firefox documents and sockets are destroyed."""
import http.server
import json
import os
import threading
from pathlib import Path
from urllib.parse import urlsplit
from boot_probe import run_probe
from test_firefox_responsiveness_boot import INIT, prepare_desktop_disk, assert_cpu_recovery


def main():
    state = {'closed': 0, 'complete': False}
    parent = b'''<!doctype html><title>Page teardown regression</title><script>
let n=0;
function cycle(){let w=open('/child?n='+n,'child'+n);if(!w){fetch('/blocked');return;}
setTimeout(()=>{w.close();fetch('/closed?n='+n);if(++n<8)setTimeout(cycle,3000);else fetch('/complete');},5000);}
setTimeout(cycle,10000);</script>'''
    child = b'''<!doctype html><title>Network and memory teardown</title><script>
let keep=[];for(let i=0;i<16;i++){let b=new Uint8Array(1024*1024);b.fill(i);keep.push(b)}
setInterval(()=>{for(let i=0;i<8;i++)fetch('/data?'+Math.random()).then(r=>r.arrayBuffer()).catch(()=>{});},100);
</script>'''
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def do_GET(self):
            path = urlsplit(self.path).path
            if path == '/closed':
                state['closed'] += 1
                print('PAGE_CLOSE: '+self.path, flush=True)
            if path == '/complete': state['complete'] = True
            if path == '/blocked': print('PAGE_CLOSE: popup blocked', flush=True)
            body = parent if path == '/' else child if path == '/child' else b'x'*65536 if path == '/data' else b'ok'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Content-Type', 'text/html' if path in ('/', '/child') else 'application/octet-stream')
            self.end_headers()
            try: self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError): pass
        def log_message(self, *args): pass
    server = http.server.ThreadingHTTPServer(('127.0.0.1',0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    init = INIT.replace('FIREFOX_RESPONSIVENESS_BOOT', 'FIREFOX_PAGE_CLOSE_BOOT')
    init = init.replace('180000', '120000').replace('window>=17', 'window>=11')
    init = init.replace('dump("/proc/tlbstats");', 'dump("/proc/tlbstats");dump("/proc/stat");')
    init = init.replace('security.sandbox.content.level\\\",0);', 'security.sandbox.content.level\\\",0);\\nuser_pref(\\\"dom.disable_open_during_load\\\",false);')
    try:
        out = run_probe(os.environ.get('AVORY_CLOSE_TEST_NAME','firefox-page-close-boot'), init.replace('__PORT__',str(server.server_port)),
            'FIREFOX_PAGE_CLOSE_BOOT: PASS', cpus=8, memory='4G', timeout=240,
            prepare_disk=prepare_desktop_disk, kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init',
            qemu_args=['-netdev','user,id=net0','-device','rtl8139,netdev=net0'])
        (out/'page-close-metrics.json').write_text(json.dumps(state,indent=2))
        if state['closed'] != 8 or not state['complete']:
            raise RuntimeError(f'Browser did not close eight loaded documents: {state}')
        assert_cpu_recovery((out/'serial.log').read_text(errors='replace'))
    finally:
        server.shutdown()
        server.server_close()

if __name__ == '__main__': main()
