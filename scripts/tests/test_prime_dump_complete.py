#!/usr/bin/env python3
"""A perf request must print its entire new report, not the previous tail."""
from pathlib import Path
import contextlib
import io
import runpy
import tempfile
from unittest.mock import patch
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='prime-perf-complete-') as tmp:
 log=Path(tmp)/'serial.log'
 for rotated in (False,True):
  log.write_text('old report\n'*100)
  sent=[];tick=[0];completed=[False]
  class Socket:
   def __init__(self,*args):pass
   def __enter__(self):return self
   def __exit__(self,*args):pass
   def settimeout(self,*args):pass
   def connect(self,*args):pass
   def sendall(self,data):
    sent.append(data)
    with log.open('w' if rotated else 'a') as stream:
     stream.write('[GPUPERF] snapshot_ns=42 enabled=1\n[GPUPERF] syncobj_ready_fast=1\n')
  def sleep(_):
   if not completed[0]:
    with log.open('a') as stream:stream.write('[GPUPERF] syncobj_sleep caller=glxgears parks=1\n[GPUPERF] end\n')
    completed[0]=True
  def now():tick[0]+=0.01;return tick[0]
  out=io.StringIO()
  with patch('socket.socket',Socket),patch('time.sleep',sleep),patch('time.monotonic',now),patch('sys.argv',['prime-dump.py','--perf','--wait','--log',str(log)]),contextlib.redirect_stdout(out):
   runpy.run_path(str(root/'scripts/prime-dump.py'),run_name='__main__')
  result=out.getvalue()
  assert sent==[b'gpup\n'] and completed[0]
  assert '[GPUPERF] end' in result and 'caller=glxgears' in result and 'old report' not in result
print('PASS: complete fresh timing report, partial writes and log truncation; no hang-dump request')
