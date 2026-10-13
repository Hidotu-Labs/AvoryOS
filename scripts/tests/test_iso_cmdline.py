#!/usr/bin/env python3
"""Check the actual ISO recipe replaces a command line without duplicate keys."""
from pathlib import Path
import re
import subprocess
root=Path(__file__).resolve().parents[2]
recipe=(root/'GNUmakefile').read_text()
program=re.search(r"awk -v c='\$\(KERNEL_CMDLINE\)' \\\n\s*'([^']+)'",recipe).group(1)
config=(root/'limine.conf').read_text()
setting='init=/sbin/init splash quiet wine_loader_trace'
def render(text):
 return subprocess.run(['awk','-v','c='+setting,program],input=text,text=True,
                       check=True,capture_output=True).stdout
original=re.split(r'(?m)(?=^/)',config)
result=re.split(r'(?m)(?=^/)',render(config))
assert len(original)==len(result)
selected=False
for before,after in zip(original,result):
 if before.startswith('/AvoryOS') and not selected:
  selected=True
  values=re.findall(r'(?m)^\s*cmdline:\s*(.*)$',after)
  assert values==[setting],values
  missing=re.sub(r'(?m)^\s*cmdline:.*\n','',before)
  assert re.findall(r'(?m)^\s*cmdline:\s*(.*)$',render(missing))==[setting]
 else:assert before.splitlines()==after.splitlines()
assert selected
print('PASS: primary boot command line replaced exactly once; init preserved and other menu entries unchanged')
