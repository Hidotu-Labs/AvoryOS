#!/usr/bin/env python3
"""Compare native wire structs to Linux UAPI without Linux kernel internals."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
code='''#include <linux/fuse.h>\n#include <stddef.h>\n#include <stdio.h>\n#include "kernel/src/fs/fuse_protocol.h"\n'''
for name in ['in_header','out_header','init_out','attr','entry_out','attr_out','getattr_in','open_in','open_out','read_in','release_in','forget_in','kstatfs','dirent']:
    code+=f'_Static_assert(sizeof(struct afuse_{name})==sizeof(struct fuse_{name}),"{name} size");\n'
for name,field in [('in_header','unique'),('in_header','nodeid'),('in_header','pid'),('out_header','unique'),('attr','mode'),('attr','uid'),('entry_out','attr'),('read_in','size'),('read_in','flags'),('dirent','name')]:
    code+=f'_Static_assert(offsetof(struct afuse_{name},{field})==offsetof(struct fuse_{name},{field}),"{name}.{field}");\n'
code+='int main(void){puts("PASS: FUSE protocol structure sizes and offsets match Linux UAPI");}\n'
with tempfile.TemporaryDirectory(prefix='avory-fuse-abi-') as d:
    p=Path(d);(p/'test.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(ROOT),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
