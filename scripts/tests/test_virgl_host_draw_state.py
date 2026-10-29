#!/usr/bin/env python3
"""Exercise actual host program caching and direct/indirect draw bindings."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
s=(root/'build/virgl-host-src/src/vrend/vrend_renderer.c').read_text()
program=s[s.index('static void vrend_make_current('):s.index('static void vrend_depth_test_enable(')]
draw_start=s.index('int vrend_draw_vbo(')
begin=s.index('   if (indirect_res) {\n      glBindBuffer(GL_DRAW_INDIRECT_BUFFER',draw_start)
end=s.index('   if (info->vertices_per_patch',begin)
indirect=s[begin:end]
destroy=s[s.index('static void vrend_destroy_program(struct vrend_linked_shader_program *ent)\n{'):s.index('static void vrend_destroy_program(struct vrend_linked_shader_program *ent)\n{')+900]
assert destroy.index('program_binding_valid = false')<destroy.index('glDeleteProgramPipelines')
# Every render-thread context switch, including blitter transitions, uses the
# invalidating wrapper. The fence thread has a separate surfaceless GL context.
assert s.count('vrend_clicbs->make_current(')==1
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
typedef unsigned GLuint;typedef int virgl_gl_context;
struct vrend_linked_shader_program {bool is_pipeline;union {GLuint program,pipeline;}id;};
static struct {bool program_binding_valid,bound_program_is_pipeline;GLuint bound_program_id;}vrend_state;
static unsigned program_calls,pipeline_calls,buffer_calls,context_calls;
static GLuint active_program,active_pipeline,argument_buffer,count_buffer;
static bool separate=true;
enum {feat_separate_shader_objects,GL_DRAW_INDIRECT_BUFFER,GL_PARAMETER_BUFFER_ARB};
static bool has_feature(int f){(void)f;return separate;}
static void glUseProgram(GLuint id){program_calls++;active_program=id;}
static void glBindProgramPipeline(GLuint id){pipeline_calls++;active_pipeline=id;}
static void make_current(virgl_gl_context ctx){(void)ctx;context_calls++;active_program=active_pipeline=0;}
static struct {void (*make_current)(virgl_gl_context);}callbacks={make_current},*vrend_clicbs=&callbacks;
static void glBindBuffer(unsigned target,GLuint id){buffer_calls++;if(target==GL_DRAW_INDIRECT_BUFFER)argument_buffer=id;else{assert(target==GL_PARAMETER_BUFFER_ARB);count_buffer=id;}}
struct resource {GLuint gl_id;};
'''
main=r'''
static void bind_arguments(struct resource *indirect_res,struct resource *indirect_params_res){
 INDIRECT
}
int main(void){
 struct vrend_linked_shader_program a={.id.program=7},b={.id.program=8},pipeline={.is_pipeline=true,.id.pipeline=7};
 vrend_make_current(1);vrend_use_program(&a);assert(program_calls==1&&pipeline_calls==1&&active_program==7&&!active_pipeline);
 for(unsigned i=0;i<180000;i++){vrend_use_program(&a);bind_arguments(NULL,NULL);}
 assert(program_calls==1&&pipeline_calls==1&&!buffer_calls);
 vrend_use_program(&b);assert(program_calls==2&&pipeline_calls==2&&active_program==8);
 vrend_use_program(&pipeline);assert(program_calls==3&&pipeline_calls==3&&!active_program&&active_pipeline==7);
 vrend_use_program(&pipeline);assert(program_calls==3);
 vrend_use_program(&a);assert(program_calls==4&&active_program==7&&!active_pipeline);
 vrend_use_program(NULL);assert(program_calls==5&&!active_program&&!active_pipeline);
 vrend_use_program(NULL);assert(program_calls==5);
 /* Identical numeric GL names in another context must still be rebound. */
 vrend_use_program(&a);vrend_make_current(2);vrend_use_program(&a);assert(program_calls==7&&context_calls==2&&active_program==7);
 /* Returning from the blitter invalidates the renderer's cache too. */
 vrend_make_current(99);glUseProgram(123);vrend_make_current(2);vrend_use_program(&a);assert(active_program==7);
 /* Program deletion invalidates even if the subsequent object reuses its name. */
 vrend_state.program_binding_valid=false;active_program=0;unsigned before=program_calls;
 vrend_use_program(&a);assert(active_program==7&&program_calls==before+1);
 separate=false;vrend_make_current(3);before=pipeline_calls;vrend_use_program(&a);vrend_use_program(&a);assert(pipeline_calls==before);
 struct resource args={41},count={42};
 bind_arguments(&args,NULL);assert(buffer_calls==1&&argument_buffer==41);
 bind_arguments(NULL,NULL);assert(buffer_calls==1&&argument_buffer==41);
 bind_arguments(&args,&count);assert(buffer_calls==3&&argument_buffer==41&&count_buffer==42);
 /* Transfer paths may change these targets: indirect draws always rebind. */
 argument_buffer=99;count_buffer=98;bind_arguments(&args,&count);
 assert(buffer_calls==5&&argument_buffer==41&&count_buffer==42);
 puts("PASS: 180000 direct draws avoid redundant program/pipeline and indirect-target binds; program changes, context/blitter switches, deletion/name reuse and indirect draws preserve state");
}
'''.replace(' INDIRECT',indirect)
with tempfile.TemporaryDirectory(prefix='virgl-draw-state-') as tmp:
    p=Path(tmp);code=pre+program+main;(p/'test.c').write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
    mutant=code.replace('   vrend_state.program_binding_valid = false;\n   vrend_clicbs->make_current(context);','   vrend_clicbs->make_current(context);')
    assert mutant!=code;(p/'mutant.c').write_text(mutant)
    subprocess.run(['cc','-O2',str(p/'mutant.c'),'-o',str(p/'mutant')],check=True)
    assert subprocess.run([str(p/'mutant')],capture_output=True).returncode!=0
    print('PASS: omitting context-switch invalidation fails the GL state regression')
