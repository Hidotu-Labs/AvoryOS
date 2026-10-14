#!/usr/bin/env python3
"""Compare fault bitmasks to the pinned Linux tree and test first-retry logic."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/linuxkpi/include/linux/mm.h').read_text()
up=(root/'kernel/linux/include/linux/mm_types.h').read_text()
values=re.findall(r'^#define (VM_FAULT_\w+) (0x[0-9a-fA-F]+)',s,re.M)
assert values
for name,value in values:
 match=re.search(r'\b'+name+r'\s*=\s*\(__force vm_fault_t\)(0x[0-9a-fA-F]+)',up)
 assert match and int(value,16)==int(match[1],16),name
start=s.index('static inline bool fault_flag_allow_retry_first(');end=s.index('\n}',start)+2
function=s[start:end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define FAULT_FLAG_ALLOW_RETRY 4
#define FAULT_FLAG_TRIED 32
'''+function+r'''
int main(void){
 assert(!fault_flag_allow_retry_first(0));assert(fault_flag_allow_retry_first(4));
 assert(!fault_flag_allow_retry_first(32));assert(!fault_flag_allow_retry_first(4|32));
 puts("PASS: Linux 6.6 fault masks and retry only on the first allowed attempt");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-fault-contract-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 old=code.replace('&& !(flags & FAULT_FLAG_TRIED)','&& (flags & FAULT_FLAG_TRIED)')
 (p/'old.c').write_text(old)
 subprocess.run(['cc','-O2',str(p/'old.c'),'-o',str(p/'old')],check=True)
 r=subprocess.run([str(p/'old')],capture_output=True);assert r.returncode!=0
 print('PASS: former inverted first-retry predicate fails the regression')
