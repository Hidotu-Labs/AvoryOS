#!/usr/bin/env python3
"""Compile the native init= extraction block and check argument boundaries."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/src/kernel.c').read_text()
start=s.index('  char custom_init[128] = {0};')
end=s.index('\n  while (1) {',start)
block=s[start:end]
source='''#include <assert.h>
#include <string.h>
#include <stddef.h>
static const char *kernel_boot_cmdline;
static void parse(const char *input,char *output){kernel_boot_cmdline=input;
'''+block+r'''strcpy(output,custom_init);}
int main(void){char output[128];
 const char *inputs[]={"init=/sbin/init", "kpi_amdgpu_psp_reinit=1 init=/bin/probe-init", "notinit=/wrong\tinit=/right", "prefixinit=/wrong", "x=init=/wrong\ninit=/right", "init=/first init=/second", "noinit=1", "\rinit=/right", "init=", NULL};
 const char *expected[]={"/sbin/init","/bin/probe-init","/right","","/right","/first","","/right","",""};
 for(unsigned i=0;i<10;i++){parse(inputs[i],output);assert(!strcmp(output,expected[i]));}
}
'''
with tempfile.TemporaryDirectory(prefix='native-init-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(source)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: actual native init= extraction respects argument boundaries, whitespace, embedded keys, first key and null command line')
