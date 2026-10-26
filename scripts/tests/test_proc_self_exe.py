#!/usr/bin/env python3
"""Check the actual exec path snapshot helper and its pre-commit placement."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'kernel/src/syscalls/sys_process_exec.c').read_text()
a=source.index('static bool exec_save_absolute_path(')
b=source.index('{',a)+1;depth=1
while depth:
    depth+=(source[b]=='{')-(source[b]=='}');b+=1
helper=source[a:b]
# Keep the saved filename separate from the caller's argv/AT_EXECFN path.
assert source.index('exec_save_absolute_path(saved_exe_path') < source.index('vmm_create_pml4()')
assert source.index('strcpy(current->exe_path, saved_exe_path)') > source.index('if (!elf_load(')
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
'''+helper+r'''
int main(void){
 char out[256];
 assert(exec_save_absolute_path(out,sizeof(out),"..//Downloads/Forkit-1.3.0.AppImage","/"));
 assert(!strcmp(out,"/..//Downloads/Forkit-1.3.0.AppImage"));
 assert(exec_save_absolute_path(out,sizeof(out),"./Forkit.AppImage","/Downloads"));
 assert(!strcmp(out,"/Downloads/./Forkit.AppImage"));
 /* A symlink component followed by .. must be resolved by VFS, not collapsed. */
 assert(exec_save_absolute_path(out,sizeof(out),"link/../app","/launch"));
 assert(!strcmp(out,"/launch/link/../app"));
 assert(exec_save_absolute_path(out,sizeof(out),"/Downloads/app","/unrelated"));
 assert(!strcmp(out,"/Downloads/app"));
 assert(exec_save_absolute_path(out,sizeof(out),"app",NULL)&&!strcmp(out,"/app"));
 assert(exec_save_absolute_path(out,sizeof(out),"app","/bin/")&&!strcmp(out,"/bin/app"));
 strcpy(out,"unchanged");assert(!exec_save_absolute_path(out,4,"app","/"));
 assert(!strcmp(out,"unchanged"));
 assert(exec_save_absolute_path(out,5,"app","/")&&!strcmp(out,"/app"));
 assert(!exec_save_absolute_path(out,0,"app","/"));
 assert(!exec_save_absolute_path(out,sizeof(out),"","/"));
 char long_name[256];memset(long_name,'x',255);long_name[255]=0;
 assert(!exec_save_absolute_path(out,sizeof(out),long_name,"/"));
 puts("PASS: executable names anchored to exec cwd, symlink traversal preserved, exact bounds and pre-commit publication");
}
'''
with tempfile.TemporaryDirectory(prefix='avory-exe-path-') as tmp:
    src=Path(tmp)/'test.c';src.write_text(code)
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
