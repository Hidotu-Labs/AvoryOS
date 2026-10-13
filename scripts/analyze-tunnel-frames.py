#!/usr/bin/env python3
"""Find conspicuously green pixels in apitrace's tunnel snapshot PNM stream.

The default demo uses gray textures/fog and red text. This is a targeted
artifact detector, not a full rendering oracle or a general PNM parser.
"""
import argparse
import json
from pathlib import Path
import re
import numpy as np


HEADER = re.compile(rb'P6\n#(\d+)\n(\d+) (\d+)\n255\n')


def analyze(path):
    frames = 0
    bad = []
    with path.open('rb') as stream:
        while True:
            start = stream.tell()
            prefix = stream.read(128)
            if not prefix:
                break
            match = HEADER.match(prefix)
            if not match:
                raise ValueError(f'Invalid PNM header at offset {start}')
            call, width, height = map(int, match.groups())
            if not (0 < width <= 8192 and 0 < height <= 8192):
                raise ValueError('Invalid snapshot dimensions')
            stream.seek(start + match.end())
            pixels = stream.read(width * height * 3)
            if len(pixels) != width * height * 3:
                raise ValueError(f'Truncated frame at call {call}')
            rgb = np.frombuffer(pixels, dtype=np.uint8).reshape(height, width, 3).astype(np.int16)
            mask = (rgb[:, :, 1] > rgb[:, :, 0] + 40) & (rgb[:, :, 1] > rgb[:, :, 2] + 40)
            ys, xs = np.nonzero(mask)
            if len(xs):
                bad.append({'call': call, 'count': len(xs), 'examples': [
                    {'x': int(x), 'y': int(y), 'rgb': rgb[y, x].tolist()}
                    for y, x in zip(ys[:8], xs[:8])]})
            frames += 1
    return {'path': str(path), 'frames': frames, 'green_frames': len(bad), 'detections': bad}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stream', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.stream), indent=2))
