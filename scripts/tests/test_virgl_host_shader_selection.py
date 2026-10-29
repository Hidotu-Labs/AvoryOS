#!/usr/bin/env python3
"""Run host vertex-format invalidation and draw selection against chunk switches."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'build/virgl-host-src/src/vrend/vrend_renderer.c').read_text()
bind = source[source.index('void vrend_bind_vertex_elements_state('):source.index('void vrend_set_constants(')]
begin = source.index('   if (sub_ctx->shader_dirty ||', source.index('int vrend_draw_vbo('))
end = source.index('   if (!sub_ctx->prog ||', begin)
select = source[begin:end]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define VIRGL_OBJECT_VERTEX_ELEMENTS 1
#define VIRGL_ERROR_CTX_ILLEGAL_HANDLE 2
#define VIRGL_ERROR_CTX_TOO_MANY_VERTEX_ATTRIBUTES 3
#define GL_BGRA 4
#define UPDATE_INT_SIGN_MASK(fmt,i,s,u) do{(void)(fmt);(void)(i);(void)(s);(void)(u);}while(0)
typedef unsigned GLuint;typedef int GLint;
struct vrend_vertex_element {struct {unsigned src_format,src_offset,vertex_buffer_index,instance_divisor;}base;int nr_chan,type,norm;};
struct vrend_vertex_element_array {unsigned count,id,zyxw_bitmask,signed_int_bitmask,unsigned_int_bitmask;struct vrend_vertex_element elements[16];};
struct vrend_sub_context {void *object_hash;struct vrend_vertex_element_array *ve;bool vbo_dirty,shader_dirty;unsigned swizzle_output_rgb_to_bgr,needs_manual_srgb_encode_bitmask;};
struct vrend_context {struct vrend_sub_context *sub;};
static struct {bool use_gles;unsigned max_vertex_attributes;}vrend_state={false,16};
static struct vrend_vertex_element_array formats[2]={{.count=3,.id=1},{.count=3,.id=2}};
static unsigned selections,errors,buffer_binds;
static void *vrend_object_lookup(void *h,unsigned id,unsigned t){(void)h;(void)t;return id&&id<=2?&formats[id-1]:NULL;}
static void vrend_report_context_error(struct vrend_context *c,unsigned e,unsigned h){(void)c;(void)e;(void)h;errors++;}
enum {feat_gles31_vertex_attrib_binding};
static bool has_feature(int f){(void)f;return true;}
static bool util_format_is_pure_integer(unsigned f){(void)f;return false;}
static void glGenVertexArrays(int n,GLuint *id){(void)n;*id=1;}
static void glBindVertexArray(GLuint id){(void)id;}
static void glVertexAttribIFormat(unsigned i,int s,int t,unsigned o){(void)i;(void)s;(void)t;(void)o;}
static void glVertexAttribFormat(unsigned i,int s,int t,int n,unsigned o){(void)i;(void)s;(void)t;(void)n;(void)o;}
static void glVertexAttribBinding(unsigned i,unsigned b){(void)i;(void)b;}
static void glVertexBindingDivisor(unsigned i,unsigned d){(void)i;(void)d;}
static void glEnableVertexAttribArray(unsigned i){(void)i;}
static int vrend_select_program(struct vrend_sub_context *s,unsigned patches){(void)patches;selections++;s->shader_dirty=false;return 0;}
'''
main = r'''
static void draw(struct vrend_sub_context *sub_ctx){
 struct {unsigned vertices_per_patch;} draw_info={0},*info=&draw_info;
 int program_select_result=0;
 SELECTION
 (void)program_select_result;
 /* The binding dirty flag still reaches the vertex upload path. */
 if(sub_ctx->vbo_dirty){buffer_binds++;sub_ctx->vbo_dirty=false;}
}
int main(void){
 struct vrend_sub_context sub={0};struct vrend_context ctx={&sub};
 vrend_bind_vertex_elements_state(&ctx,1);assert(sub.shader_dirty&&sub.vbo_dirty);
 draw(&sub);assert(selections==1&&buffer_binds==1);
 for(unsigned frame=0;frame<10;frame++)for(unsigned chunk=0;chunk<686;chunk++){
  /* Changing only the VBO identity/offset must still upload vertex bindings
   * without rebuilding identical shader keys for each visible chunk. */
  sub.vbo_dirty=true;draw(&sub);
 }
 assert(selections==1&&buffer_binds==6861);
 vrend_bind_vertex_elements_state(&ctx,1);draw(&sub);assert(selections==1);
 vrend_bind_vertex_elements_state(&ctx,2);draw(&sub);assert(selections==2&&buffer_binds==6862);
 vrend_bind_vertex_elements_state(&ctx,0);assert(!sub.ve&&sub.shader_dirty);draw(&sub);assert(selections==3);
 vrend_bind_vertex_elements_state(&ctx,0);draw(&sub);assert(selections==3);
 vrend_bind_vertex_elements_state(&ctx,1);draw(&sub);assert(selections==4);
 vrend_bind_vertex_elements_state(&ctx,99);assert(errors==1&&sub.ve==&formats[0]&&!sub.shader_dirty);
 sub.shader_dirty=true;draw(&sub);assert(selections==5);
 sub.swizzle_output_rgb_to_bgr=1;draw(&sub);assert(selections==6);sub.swizzle_output_rgb_to_bgr=0;
 sub.needs_manual_srgb_encode_bitmask=1;draw(&sub);assert(selections==7);
 puts("PASS: 6860 buffer switches avoid shader reselection; format/unbind/restore, explicit shader and framebuffer invalidation still select shaders");
}
'''.replace(' SELECTION', select)
with tempfile.TemporaryDirectory(prefix='virgl-shader-selection-') as tmp:
    p=Path(tmp)
    code=pre+bind+main
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
    mutants = [code.replace('sub_ctx->needs_manual_srgb_encode_bitmask)', 'sub_ctx->needs_manual_srgb_encode_bitmask || sub_ctx->vbo_dirty)'),
               code.replace('ctx->sub->shader_dirty = true;', 'ctx->sub->shader_dirty = false;')]
    for i,mutant in enumerate(mutants):
        assert mutant!=code
        (p/'mutant.c').write_text(mutant)
        subprocess.run(['cc','-O2',str(p/'mutant.c'),'-o',str(p/'mutant')],check=True)
        assert subprocess.run([str(p/'mutant')],capture_output=True).returncode!=0
    print('PASS: redundant buffer-triggered selection and missing format invalidation both fail regression')
