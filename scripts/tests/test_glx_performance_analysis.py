#!/usr/bin/env python3
"""Stalled swaps and partial trailing reports must not distort attribution."""
from pathlib import Path
import runpy
analyze=runpy.run_path(str(Path(__file__).resolve().parents[1]/'analyze-glx-performance.py'))['analyze']
trace='''FPS_TRACE mode=normal frames=5000 wall_ns=5000000000 swap_ns=4000000000 pending_ns=0
IOCTL nr=52 calls=5000 ns=50000000 max=10000
IOCTL nr=ca calls=25000 ns=3000000000 max=1000000
FPS_TRACE mode=normal frames=10000 wall_ns=10000000000 swap_ns=8000000000 pending_ns=0
IOCTL nr=52 calls=10000 ns=100000000 max=10000
IOCTL nr=ca calls=50000 ns=6000000000 max=1000000
FPS_TRACE mode=normal frames=10000 wall_ns=15000000000 swap_ns=8000000000 pending_ns=5000000000
IOCTL nr=52 calls=10000 ns=100000000 max=10000
IOCTL nr=ca calls=50000 ns=6000000000 max=1000000
'''
r=analyze(trace)
assert r['summary']['fps']==1000 and r['summary']['excluded_intervals']==1
assert r['summary']['swap_us_per_frame']==800
wait=r['intervals'][0]['ioctls']['ca']
assert wait['calls_per_frame']==5 and wait['wall_us_per_frame']==600
# The final reporter can be killed after writing its header and first ioctl.
partial=trace.split('FPS_TRACE mode=normal frames=10000 wall_ns=15000000000')[0]
partial=partial.rsplit('IOCTL nr=ca',1)[0]
r=analyze(partial)
assert r['intervals'][0]['partial_counters'] and 'ca' not in r['intervals'][0]['ioctls']
assert r['summary']['fps']==1000
# A new process/trace segment must not manufacture negative work or averages.
reset=trace+'FPS_TRACE mode=render-only frames=1 wall_ns=10 swap_ns=1 pending_ns=0\n'
assert analyze(reset)['intervals'][-1]['status']=='counter_reset_or_mode_change'
print('PASS: cumulative deltas, stalled swaps, truncated counters and process resets')
