#!/usr/bin/env python3
"""Run the actual boot parameter parser with colliding driver parameter names."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root/'kernel/linuxkpi/include/linux/moduleparam.h').read_text()
header = re.sub(r'^#include.*$', '', header, flags=re.M)
source = (root/'kernel/linuxkpi/src/params.c').read_text()
source = re.sub(r'^#include.*$', '', source, flags=re.M)
source = re.sub(r'^extern const struct kpi_(?:param|setup).*?;\n', '', source, flags=re.M)
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef int8_t s8; typedef int16_t s16; typedef uint16_t u16;
#define GFP_KERNEL 0
#define kmalloc(n,f) malloc(n)
#define kfree(p) free(p)
static unsigned warnings;
#define klogf(...) ((void)warnings++)
static const char *linuxkpi_boot_cmdline(void) { return ""; }
'''
fixtures = r'''
static unsigned long drm_debug;
static char *nouveau_debug;
static int amd_modeset, nouveau_modeset, amd_runpm, nouveau_runpm;
static bool probe_gate;
#undef KBUILD_MODNAME
#define KBUILD_MODNAME "drm"
static const struct kpi_param *drm_metadata(void) {
 module_param_named(debug, drm_debug, ulong, 0);
 return &__kpi_param_debug;
}
#undef KBUILD_MODNAME
#define KBUILD_MODNAME "nouveau"
static const struct kpi_param *nouveau_metadata(void) {
 module_param_named(debug, nouveau_debug, charp, 0);
 return &__kpi_param_debug;
}
static struct kpi_param parameters[] = {
 {"debug", &drm_debug, KPI_PARAM_ulong, 0, 0, "drm"},
 {"debug", &nouveau_debug, KPI_PARAM_charp, 0, 0, "nouveau"},
 {"modeset", &amd_modeset, KPI_PARAM_int, 0, 0, "amdgpu"},
 {"modeset", &nouveau_modeset, KPI_PARAM_int, 0, 0, "nouveau"},
 {"runpm", &amd_runpm, KPI_PARAM_int, 0, 0, "amdgpu"},
 {"runpm", &nouveau_runpm, KPI_PARAM_int, 0, 0, "nouveau"},
 {"kpi_nouveau", &probe_gate, KPI_PARAM_bool, 0, 0, "avoryos"},
};
#define __kpi_param_start parameters
#define __kpi_param_end (parameters + sizeof(parameters)/sizeof(parameters[0]))
static struct kpi_setup no_setups[1];
#define __kpi_setup_start no_setups
#define __kpi_setup_end no_setups
'''
main = r'''
int main(void) {
 assert(!strcmp(drm_metadata()->module,"drm") && drm_metadata()->type==KPI_PARAM_ulong);
 assert(!strcmp(nouveau_metadata()->module,"nouveau") && nouveau_metadata()->type==KPI_PARAM_charp);
 linuxkpi_param_parse("nouveau.debug=info drm.debug=0x4 amdgpu.modeset=1 nouveau.modeset=0 amdgpu.runpm=-1 nouveau.runpm=0 kpi_nouveau=0");
 assert(drm_debug==4 && !strcmp(nouveau_debug,"info"));
 assert(amd_modeset==1 && nouveau_modeset==0 && amd_runpm==-1 && nouveau_runpm==0 && !probe_gate);
 unsigned before=warnings;
 linuxkpi_param_parse("debug=9 modeset=9 runpm=9");
 assert(warnings==before+3 && drm_debug==4 && !strcmp(nouveau_debug,"info") && amd_modeset==1 && amd_runpm==-1);
 linuxkpi_param_parse("unknown.debug=8 unknown.runpm=8");
 assert(drm_debug==4 && amd_runpm==-1 && nouveau_runpm==0);
 linuxkpi_param_parse("nouveau.debug=trace drm.debug=0 kpi_nouveau=1");
 assert(drm_debug==0 && !strcmp(nouveau_debug,"trace") && probe_gate);
 free(nouveau_debug);
 puts("PASS: driver-qualified typed parameters, ambiguous bare names, unknown namespace and independent probe gate");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-params-') as tmp:
    path = Path(tmp)/'test'
    path.with_suffix('.c').write_text(shim+header+fixtures+source+main)
    subprocess.run(['cc','-std=gnu11','-fsanitize=address,undefined',
                    str(path.with_suffix('.c')),'-o',str(path)],check=True)
    subprocess.run([str(path)],check=True,timeout=10)
