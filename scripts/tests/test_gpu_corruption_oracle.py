#!/usr/bin/env python3
"""Validate the real fog-free GL oracle, CSV and PNG captures using llvmpipe."""
from pathlib import Path
import csv,struct,subprocess,tempfile,zlib
root=Path(__file__).resolve().parents[2]
def png(path):
 data=path.read_bytes();assert data[:8]==b'\x89PNG\r\n\x1a\n';pos=8;compressed=b'';ended=False
 while pos<len(data):
  length=struct.unpack('!I',data[pos:pos+4])[0];kind=data[pos+4:pos+8];payload=data[pos+8:pos+8+length];crc=struct.unpack('!I',data[pos+8+length:pos+12+length])[0]
  assert zlib.crc32(kind+payload)&0xffffffff==crc,(path,kind)
  if kind==b'IHDR':assert struct.unpack('!IIBBBBB',payload)==(256,256,8,6,0,0,0)
  if kind==b'IDAT':compressed+=payload
  if kind==b'IEND':ended=True
  pos+=length+12
 assert ended and pos==len(data)
 raw=zlib.decompress(compressed);assert len(raw)==256*1025
 assert all(raw[y*1025]==0 for y in range(256))
 return b''.join(raw[y*1025+1:(y+1)*1025] for y in range(256))
with tempfile.TemporaryDirectory(prefix='gpu-corruption-oracle-') as tmp:
 p=Path(tmp);binary=p/'oracle'
 flags=subprocess.check_output(['pkg-config','--cflags','--libs','egl','gbm','gl','libdrm','zlib'],text=True).split()
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(root/'userland/test_gpu_corruption.c'),'-o',str(binary),*flags],check=True)
 for integer in [False,True]:
  for case,frames in [('mixed',100),('texture',50),('stream',50)]:
   directory=p/(case+('-integer' if integer else '-normalized'))
   r=subprocess.run([str(binary),'--software','--frames',str(frames),'--case',case,'--output',str(directory),*(['--integer-source'] if integer else [])],capture_output=True,text=True,timeout=60)
   assert r.returncode==0,r.stdout+r.stderr
   assert 'Renderer: llvmpipe' in r.stdout
   rows=list(csv.DictReader((directory/'results.csv').open()));assert len(rows)==frames
   assert all(row['bad_pixels']=='0' and row['actual_crc32']==row['expected_crc32'] for row in rows)
   assert not list(directory.glob('*.png'))
   print(f'PASS: {case} {"integer" if integer else "normalized"} {frames} llvmpipe frames ({frames*256*256} exact pixel checks), zero mismatches')
 directory=p/'injected'
 r=subprocess.run([str(binary),'--software','--frames','4','--inject-readback-error','2','--output',str(directory)],capture_output=True,text=True,timeout=20)
 assert r.returncode==1 and 'TEST-ONLY' in r.stderr,r.stdout+r.stderr
 rows=list(csv.DictReader((directory/'results.csv').open()));assert [int(row['bad_pixels']) for row in rows]==[0,0,1,0]
 actual=png(directory/'frame-000002-actual.png');expected=png(directory/'frame-000002-expected.png')
 # GL lower-left (23,17) becomes PNG top-left (23,238).
 offset=((255-17)*256+23)*4
 differences=[i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b];assert differences==[offset]
 assert actual[offset]==expected[offset]^255
 print('PASS: test-only injected readback mismatch returns failure, correct frame CSV and valid top-down actual/expected PNG pair')
 # Missing AMD is a setup error, not a false success/software fallback.
 vendor=(Path('/sys/class/drm/renderD128/device/vendor').read_text().strip() if Path('/sys/class/drm/renderD128/device/vendor').exists() else '')
 if vendor=='0x10de':
  r=subprocess.run([str(binary),'--frames','1','--output',str(p/'hardware')],capture_output=True,text=True,timeout=10)
  assert r.returncode==2 and 'no AMDGPU render node' in r.stderr
  print('PASS: unavailable AMDGPU hardware returns an explicit unexecuted-test error')
