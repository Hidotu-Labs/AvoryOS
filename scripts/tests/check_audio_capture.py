#!/usr/bin/env python3
"""Check QEMU HDA's captured 440-Hz stereo S16 tone for gaps and clicks.

Use AVORY_AUDIO_SECONDS=20 AVORY_AUDIO_CAPTURE=1 AVORY_AUDIO_PULSE=1 with
 test_udev_audio_boot.py first. QEMU may leave RIFF/data sizes zero on guest
power-off; parse the PCM payload using its format chunk and actual file length.
"""
from pathlib import Path
import argparse,array,json,math,struct
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('capture',type=Path)
p.add_argument('--seconds',type=float,default=20)
a=p.parse_args()
b=a.capture.read_bytes()
assert b[:4]==b'RIFF' and b[8:12]==b'WAVE'
pos=12;fmt=None;payload=None
while pos+8<=len(b):
 kind=b[pos:pos+4];size=struct.unpack_from('<I',b,pos+4)[0];pos+=8
 if kind==b'fmt ':fmt=struct.unpack_from('<HHIIHH',b,pos)
 if kind==b'data':payload=b[pos:pos+size] if size else b[pos:];break
 pos+=size+(size&1)
assert fmt and payload is not None
encoding,channels,rate,_,align,bits=fmt
assert encoding==1 and channels==2 and bits==16 and align==4
samples=array.array('h',payload)
import sys
if sys.byteorder!='little':samples.byteswap()
left=samples[::2];active=[i for i,v in enumerate(left) if abs(v)>50]
assert active,'no tone captured'
start,end=active[0],active[-1]
# Ignore only onset/termination transitions, retaining all of steady playback.
lo=start+int(rate*.2);hi=end-int(rate*.2)
assert hi>lo
runs=[];quiet=0;clicks=0;peak=max(abs(v) for v in left[lo:hi])
# A 440-Hz sinusoid's largest natural step is 2*A*sin(pi*f/rate).
step_limit=max(32,3*2*peak*math.sin(math.pi*440/rate))
for i in range(lo,hi):
 if abs(left[i])<5:quiet+=1
 else:
  if quiet>rate/1000:runs.append(quiet)
  quiet=0
 if i>lo and abs(left[i]-left[i-1])>step_limit:clicks+=1
if quiet>rate/1000:runs.append(quiet)
result={'rate':rate,'tone_span_seconds':(end-start+1)/rate,
        'gap_count_over_1ms':len(runs),'gap_seconds':sum(runs)/rate,
        'discontinuities':clicks,'peak':peak}
print(json.dumps(result,indent=2))
assert abs(result['tone_span_seconds']-a.seconds)<.15,'playback duration drift'
assert not runs,'silent holes inside tone'
assert not clicks,'sample discontinuities inside tone'
