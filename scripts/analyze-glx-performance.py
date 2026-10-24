#!/usr/bin/env python3
"""Analyze cumulative FPS_TRACE snapshots without counting stalls as steady FPS.

These traces come from the optional glXSwapBuffers/glFinish probe retained in
build/nvidia-fps. IOCTL timers sum caller wall time, possibly across threads;
they must not be added to swap time or treated as GPU execution time.
"""
import argparse
import json
from pathlib import Path
import re

HEADER = re.compile(r'FPS_TRACE mode=(\S+) frames=(\d+) wall_ns=(\d+) swap_ns=(\d+) pending_ns=(\d+)')
IOCTL = re.compile(r'IOCTL nr=([0-9a-f]+) calls=(\d+) ns=(\d+) max=(\d+)')
NAMES = {'51': 'Nouveau VM_BIND', '52': 'Nouveau EXEC',
         'ca': 'DRM SYNCOBJ_TIMELINE_WAIT', 'c3': 'DRM SYNCOBJ_TRANSFER'}


def analyze(text):
    headers = list(HEADER.finditer(text))
    snapshots = []
    for i, match in enumerate(headers):
        mode, frames, wall, swap, pending = match.groups()
        end = headers[i + 1].start() if i + 1 < len(headers) else len(text)
        costs = {nr: {'calls': int(c), 'ns': int(ns)}
                 for nr, c, ns, _ in IOCTL.findall(text[match.end():end])}
        snapshots.append(dict(mode=mode, frames=int(frames), wall=int(wall),
                              swap=int(swap), pending=int(pending), costs=costs))
    intervals = []
    for before, after in zip(snapshots, snapshots[1:]):
        frames = after['frames'] - before['frames']
        wall = after['wall'] - before['wall']
        swap = after['swap'] - before['swap']
        if before['mode'] != after['mode'] or min(frames, swap) < 0 or wall <= 0:
            intervals.append({'status': 'counter_reset_or_mode_change'})
            continue
        # A snapshot can land in the middle of a stuck swap. Its duration is
        # absent from completed swap_ns; do not mislabel it as rendering work.
        stalled = not frames or max(before['pending'], after['pending']) >= 100_000_000
        row = dict(status='stalled_or_inflight' if stalled else 'steady',
                   frames=frames, wall_ns=wall, swap_ns=swap,
                   fps=frames * 1e9 / wall,
                   pending_ms=after['pending'] / 1e6,
                   partial_counters=not set(before['costs']).issubset(after['costs']),
                   ioctls={})
        if frames:
            row['wall_us_per_frame'] = wall / frames / 1000
            row['swap_us_per_frame'] = swap / frames / 1000
            for nr, old in before['costs'].items():
                new = after['costs'].get(nr)
                if new is None:
                    continue
                calls, elapsed = new['calls'] - old['calls'], new['ns'] - old['ns']
                if calls < 0 or elapsed < 0:
                    continue
                row['ioctls'][nr] = dict(name=NAMES.get(nr, 'other'), calls=calls,
                    wall_ns=elapsed, calls_per_frame=calls / frames,
                    wall_us_per_frame=elapsed / frames / 1000)
        intervals.append(row)
    steady = [row for row in intervals if row['status'] == 'steady']
    frames = sum(row['frames'] for row in steady)
    wall = sum(row['wall_ns'] for row in steady)
    summary = dict(snapshots=len(snapshots), steady_intervals=len(steady),
                   excluded_intervals=len(intervals) - len(steady))
    if frames:
        summary.update(fps=frames * 1e9 / wall,
                       wall_us_per_frame=wall / frames / 1000,
                       swap_us_per_frame=sum(row['swap_ns'] for row in steady) / frames / 1000)
    return dict(summary=summary, intervals=intervals)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('traces', type=Path, nargs='+')
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    result = {str(path): analyze(path.read_text(errors='replace')) for path in args.traces}
    for path, data in result.items():
        s = data['summary']
        timing = (f"{s['fps']:.0f} FPS, {s['swap_us_per_frame']:.1f} us/swap"
                  if 'fps' in s else 'no steady interval')
        print(f"{path}: {timing}; {s['excluded_intervals']} excluded intervals")
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
