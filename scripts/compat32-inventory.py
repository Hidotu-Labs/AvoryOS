#!/usr/bin/env python3
"""Classify Linux i386 syscall mappings; mapped does not imply feature parity."""
from pathlib import Path
import re, json
root=Path(__file__).resolve().parents[1]
source=(root/'kernel/src/syscalls/sys_compat.c').read_text()
source=source[source.index('static int compat_syscall_number'):source.index('/* Keep the unmodified')]
known={int(n) for n in re.findall(r'case (\d+):',source)}|{243}
linux=Path('/usr/include/asm/unistd_32.h').read_text()
rows=[{'number':int(n),'name':name,'mapped':int(n) in known} for name,n in re.findall(r'#define __NR_(\w+) (\d+)',linux)]
print(json.dumps(rows,indent=2))
