#!/usr/bin/env python3
"""Check the actual host guard permits bufferless and sparse valid vertex input."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=ROOT/'build/virgl-host-src/src/vrend/vrend_renderer.c'
if not source.exists():
    raise SystemExit('Run make virgl-host first')
s=source.read_text()
a=s.index('   /* An enabled vertex element with no backing BO')
b=s.index('   vrend_draw_bind_objects(',a)
guard=s[a:b]
pre=r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define PIPE_SHADER_VERTEX 0
#define MIN2(a,b) ((a)<(b)?(a):(b))
static unsigned warnings;
#define virgl_warn(...) (++warnings)
struct element{struct {unsigned vertex_buffer_index;} base;};
struct elements{unsigned count;struct element elements[4];};
struct selector{struct {int num_inputs;unsigned attrib_input_mask;} sinfo;};
struct shader{struct selector *sel;};
struct program{struct shader *ss[1];};
struct subcontext{struct elements *ve;struct program *prog;int num_vbos;bool warned_missing_vertex_buffer;struct{struct{void *buffer;}base;}vbo[4];};
static int draw(struct subcontext *sub_ctx){
'''
main=r'''
int main(void){
 struct selector vs={0};struct shader sh={&vs};struct program p={{&sh}};
 struct elements ve={.count=2};struct subcontext c={.ve=&ve,.prog=&p};
 assert(draw(&c)); /* gl_VertexID-only shaders have no vertex inputs. */
 vs.sinfo.num_inputs=2;vs.sinfo.attrib_input_mask=3;c.num_vbos=1;c.vbo[0].base.buffer=&vs;
 assert(draw(&c)); /* Two attributes interleaved in one buffer. */
 ve.elements[1].base.vertex_buffer_index=3;vs.sinfo.num_inputs=1;vs.sinfo.attrib_input_mask=1;
 assert(draw(&c)); /* Unused attribute must not reject an otherwise valid draw. */
 vs.sinfo.num_inputs=2;vs.sinfo.attrib_input_mask=3;c.num_vbos=4;c.vbo[3].base.buffer=&ve;
 assert(draw(&c)); /* Sparse higher binding with backing storage. */
 c.vbo[3].base.buffer=NULL;assert(!draw(&c)&&warnings==1);
 assert(!draw(&c)&&warnings==1); /* Warning is bounded per context. */
 ve.elements[1].base.vertex_buffer_index=4;assert(!draw(&c));
 c.num_vbos=0;assert(!draw(&c));
 c.ve=NULL;assert(draw(&c)); /* No enabled vertex arrays. */
 c.ve=&ve;ve.count=0;assert(draw(&c));
 /* Sparse shader inputs are register indices, not the first num_inputs
  * elements. An unused low attribute can be unbound, while an active high
  * attribute must still be checked. Both cases defeated the old guard. */
 ve.count=3;c.num_vbos=4;vs.sinfo.num_inputs=1;vs.sinfo.attrib_input_mask=4;
 ve.elements[0].base.vertex_buffer_index=1;c.vbo[1].base.buffer=NULL;
 ve.elements[2].base.vertex_buffer_index=3;c.vbo[3].base.buffer=&ve;
 assert(draw(&c));
 c.vbo[3].base.buffer=NULL;assert(!draw(&c));
 puts("PASS: missing and out-of-range buffers skipped; vertex-ID-only shaders, interleaved/sparse bindings and unused attributes preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-host-validation-') as tmp:
    p=Path(tmp);code=pre+guard+'return 1;}\n'+main
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
    old = '''      uint32_t inputs = sub_ctx->prog->ss[PIPE_SHADER_VERTEX]->sel->sinfo.attrib_input_mask;
      for (unsigned i = 0; i < sub_ctx->ve->count; i++) {
         if (!(inputs & (1u << i)))
            continue;'''
    previous = '''      unsigned inputs = sub_ctx->prog->ss[PIPE_SHADER_VERTEX]->sel->sinfo.num_inputs;
      unsigned count = MIN2(sub_ctx->ve->count, inputs);
      for (unsigned i = 0; i < count; i++) {'''
    mutant=code.replace(old,previous);assert mutant!=code
    (p/'mutant.c').write_text(mutant)
    subprocess.run(['cc','-O2',str(p/'mutant.c'),'-o',str(p/'mutant')],check=True)
    assert subprocess.run([str(p/'mutant')],capture_output=True).returncode!=0
    print('PASS: preceding input-count guard fails the sparse shader-input regression')
