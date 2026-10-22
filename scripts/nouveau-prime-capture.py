#!/usr/bin/env python3
"""Verify actual VirtIO scanout pixels, never guest memory or a generated image."""
import hashlib
import json
import re
import socket
import time
from pathlib import Path

WIDTH = 1280
HEIGHT = 800
COLORS = (0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0xff00ff, 0x00ffff, 0xffffff, 0x202020)


def expected_rgb(frame):
    if frame not in (0, 1):
        raise ValueError('unknown presentation frame')
    pixels = bytearray()
    for vertical in range(HEIGHT):
        for horizontal in range(WIDTH):
            if horizontal < 8 or vertical < 8 or horizontal >= WIDTH - 8 or vertical >= HEIGHT - 8:
                color = 0xffffff
            elif horizontal < 128 and vertical < 64:
                color = 0x00ffff if frame else 0xffff00
            else:
                color = COLORS[(horizontal * 8 // WIDTH + frame) % 8]
            pixels.extend((color >> 16, (color >> 8) & 255, color & 255))
    return bytes(pixels)


def verify_ppm(path, expected):
    data = Path(path).read_bytes()
    header = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\n', data)
    if not header or tuple(map(int, header.groups())) != (WIDTH, HEIGHT):
        raise RuntimeError('unexpected QEMU screenshot header or dimensions')
    pixels = data[header.end():]
    if len(pixels) != WIDTH * HEIGHT * 3:
        raise RuntimeError('truncated or oversized QEMU screenshot')
    if pixels != expected:
        mismatch = next(index for index, pair in enumerate(zip(pixels, expected)) if pair[0] != pair[1])
        raise RuntimeError(f'scanout mismatch at pixel={mismatch // 3} channel={mismatch % 3}')
    return hashlib.sha256(data).hexdigest()


def screendump(endpoint, destination):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(2)
        connection.connect(str(endpoint))
        with connection.makefile('rwb') as stream:
            def receive():
                line = stream.readline(1024 * 1024)
                if not line:
                    raise RuntimeError('QMP disconnected')
                return json.loads(line)

            if 'QMP' not in receive():
                raise RuntimeError('missing QMP greeting')
            for identifier, command in enumerate(({'execute':'qmp_capabilities'},
                    {'execute':'screendump', 'arguments':{'filename':str(destination.absolute()), 'format':'ppm'}})):
                stream.write((json.dumps(dict(command, id=identifier))+'\n').encode())
                stream.flush()
                while True:
                    reply = receive()
                    if reply.get('id') == identifier:
                        if 'error' in reply or 'return' not in reply:
                            raise RuntimeError(f'QMP screendump failed: {reply}')
                        break


class FrameCapture:
    def __init__(self, directory, *, gpu_producer=False, frame_count=2, pending_flips=False):
        self.directory = Path(directory)
        self.endpoint = self.directory/'qmp.sock'
        self.frames = []
        self.started = None
        self.gpu_producer = gpu_producer
        self.frame_count = frame_count
        self.pending_flips = pending_flips
        (self.directory/'capture.json').unlink(missing_ok=True)
        self.expected = [expected_rgb(frame) for frame in range(2)]

    def __call__(self, log):
        frame = len(self.frames)
        if frame == self.frame_count or f'NOUVEAU_C6_PRESENT: READY frame={frame} ' not in log:
            return
        if self.started is None:
            self.started = time.monotonic()
        destination = self.directory/f'prime-frame-{frame}.ppm'
        try:
            destination.unlink(missing_ok=True)
            screendump(self.endpoint, destination)
            digest = verify_ppm(destination, self.expected[frame % 2])
        except (OSError, RuntimeError) as error:
            if time.monotonic() - self.started > 6:
                raise RuntimeError(f'PRIME frame {frame} capture failed: {error}') from error
            return
        self.frames.append(dict(frame=frame, path=str(destination), sha256=digest, pixels=WIDTH * HEIGHT))
        self.started = None
        print(f'NOUVEAU_C6_CAPTURE: PASS frame={frame} pixels={WIDTH * HEIGHT}', flush=True)

    def finish(self):
        if len(self.frames) != self.frame_count:
            raise RuntimeError('all required PRIME scanout frames were not captured and verified')
        result = dict(status='pass', width=WIDTH, height=HEIGHT, frames=self.frames,
                      scope=('queued NVIDIA CE EXEC held on controlled dependency -> nonblocking legacy flip -> implicit producer-fence wait -> native VirtIO scanout; not engine-busy or shader rendering qualification'
                             if self.pending_flips else
                             'NVIDIA CE EXEC -> explicit timeline wait -> PRIME import -> native VirtIO scanout; no shader rendering or pending implicit-fence claim'
                             if self.gpu_producer else
                             'CPU-filled Nouveau GART -> PRIME import -> native VirtIO scanout; no GPU rendering claim'))
        (self.directory/'capture.json').write_text(json.dumps(result, indent=2)+'\n')
