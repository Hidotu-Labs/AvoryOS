#!/usr/bin/env python3
"""The guest accepts native quads only when the host advertises them."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'build/mesa-virgl/mesa-26.2.5/src/gallium/drivers/virgl/virgl_screen.c').read_text()
code=s[s.index('   caps->supported_prim_modes ='):s.index('   caps->primitive_restart =')]
pre=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define MESA_PRIM_COUNT 15
#define MESA_PRIM_QUADS 7
#define MESA_PRIM_QUAD_STRIP 8
#define BITFIELD_BIT(n) (1u<<(n))
#define BITFIELD_MASK(n) ((1u<<(n))-1)
struct screen {struct {struct {struct {uint32_t prim_mask;}v1;}caps;}caps;};
struct caps {uint32_t supported_prim_modes;};
static uint32_t get_modes(uint32_t host){struct screen screen={.caps.caps.v1.prim_mask=host},*vscreen=&screen;struct caps c={0},*caps=&c;
'''
main=r'''
return caps->supported_prim_modes;}
int main(void){uint32_t quads=BITFIELD_BIT(MESA_PRIM_QUADS),strip=BITFIELD_BIT(MESA_PRIM_QUAD_STRIP);
 assert(!(get_modes(0)&(quads|strip)));
 assert((get_modes(quads)&(quads|strip))==quads);
 assert((get_modes(strip)&(quads|strip))==strip);
 assert((get_modes(quads|strip)&(quads|strip))==(quads|strip));
 assert((get_modes(0)&~(quads|strip))==(get_modes(~0u)&~(quads|strip)));
 puts("PASS: core/GLES retain quad conversion; compatibility quads and quad strips are enabled independently from host caps");}
'''
with tempfile.TemporaryDirectory(prefix='virgl-native-quads-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(pre+code+main)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
