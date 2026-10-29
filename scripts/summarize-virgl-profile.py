#!/usr/bin/env python3
"""Summarize opt-in host Virgl dispatch timings from QEMU's stderr."""
import argparse
from collections import defaultdict
from pathlib import Path
import re


def summarize(log):
    windows = defaultdict(list)
    commands = defaultdict(lambda: defaultdict(lambda: [0, 0.0]))
    for line in log.splitlines():
        if 'AVORY_VIRGL_PROFILE ' not in line:
            continue
        fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
        if 'ctx' not in fields:
            continue
        ctx = int(fields['ctx'])
        if 'window_ms' in fields:
            window = {key: float(fields[key]) for key in
                      ('window_ms', 'dispatch_ms', 'cpu_ms', 'context_ms', 'gl_error_ms')}
            window['submissions'] = int(fields['submissions'])
            windows[ctx].append(window)
        elif 'cmd' in fields:
            item = commands[ctx][fields['cmd']]
            item[0] += int(fields['calls'])
            item[1] += float(fields['api_ms'])
    output = []
    for ctx, samples in windows.items():
        output.append(f'Context {ctx}: {len(samples)} reporting windows')
        for index, sample in enumerate(samples, 1):
            elapsed = sample['window_ms']
            if elapsed <= 0:
                continue
            output.append(
                f"  Window {index}: host dispatch {100 * sample['dispatch_ms'] / elapsed:.1f}% "
                f"of elapsed time, CPU {100 * sample['cpu_ms'] / elapsed:.1f}%, "
                f"{sample['submissions'] * 1000 / elapsed:.0f} submissions/s, "
                f"context switches {sample['context_ms']:.2f} ms, "
                f"GL error checks {sample['gl_error_ms']:.2f} ms")
        output.append('  Most expensive reported commands:')
        for name, (count, duration) in sorted(commands[ctx].items(),
                                             key=lambda item: item[1][1], reverse=True)[:8]:
            output.append(f'    {name}: {count} calls, {duration:.2f} ms in host API calls')
    if not output:
        output.append('No host dispatch samples found. Enable AVORY_VIRGL_PROFILE=1 and render for at least two seconds.')
    output.append('Times include host driver blocking and profiling overhead; they are not GPU execution times. Each window reports its eight most expensive command types.')
    return '\n'.join(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(summarize(args.log.read_text(errors='replace')))


if __name__ == '__main__':
    main()
