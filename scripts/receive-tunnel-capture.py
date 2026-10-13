#!/usr/bin/env python3
"""One-shot local receiver for the guest diagnostic helper (30 minute lifetime)."""
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import time

DEST = Path(__file__).resolve().parents[1] / 'build/gpu-diagnostics'
LIMIT = 256 * 1024 * 1024


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        names = {'/tunnel-trace': 'tunnel-guest.trace',
                 '/tunnel-log': 'tunnel-guest.log',
                 '/tunnel-replay-log': 'tunnel-guest-replay.log',
                 '/tunnel-frames': 'tunnel-guest-frames.pnm'}
        if self.path not in names:
            self.send_error(404)
            return
        try:
            length = int(self.headers.get('Content-Length', '0'))
        except ValueError:
            length = 0
        if not 0 < length <= LIMIT:
            self.send_error(413)
            return
        self.connection.settimeout(60)
        target = DEST / names[self.path]
        temporary = target.with_suffix(target.suffix + '.partial')
        try:
            with temporary.open('wb') as out:
                remaining = length
                while remaining:
                    data = self.rfile.read(min(remaining, 1024 * 1024))
                    if not data:
                        raise OSError('Incomplete upload')
                    out.write(data)
                    remaining -= len(data)
            temporary.replace(target)
        except OSError:
            temporary.unlink(missing_ok=True)
            self.send_error(400)
            return
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'Tunnel capture received on host.\n')
        self.server.received = self.path in ('/tunnel-trace', '/tunnel-frames')
        print(f'Received {length} bytes: {target}', flush=True)


if __name__ == '__main__':
    DEST.mkdir(parents=True, exist_ok=True)
    server = HTTPServer(('127.0.0.1', 9008), Handler)
    server.timeout = 1
    server.received = False
    deadline = time.monotonic() + 1800
    print('Waiting for tunnel capture on port 9008.', flush=True)
    while not server.received and time.monotonic() < deadline:
        server.handle_request()
    server.server_close()
