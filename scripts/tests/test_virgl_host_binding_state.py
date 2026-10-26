#!/usr/bin/env python3
"""Actual host VBO updates must clear bindings retired before the next draw."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root/'build/virgl-host-src/src/vrend/vrend_renderer.c').read_text()
setter = s[s.index('static void vrend_set_num_vbo_sub('):s.index('void vrend_set_num_vbo(', s.index('static void vrend_set_num_vbo_sub('))]
bind = s[s.index('static void vrend_draw_bind_vertex_binding('):s.index('static GLuint vrend_draw_bind_samplers_shader(')]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PIPE_MAX_ATTRIBS 16
#define MAX2(a,b) ((a)>(b)?(a):(b))
typedef unsigned GLuint;typedef int GLsizei;typedef intptr_t GLintptr;
struct vrend_resource {GLuint gl_id;};
struct vrend_vertex_buffer {struct {struct vrend_resource *buffer;unsigned buffer_offset,stride;}base;unsigned res_id;};
struct vrend_sub_context {int num_vbos,old_num_vbos;bool vbo_dirty;struct vrend_vertex_buffer vbo[16];};
struct vrend_context {struct vrend_sub_context *sub;};
struct vrend_vertex_element_array {GLuint id;};
enum {feat_bind_vertex_buffers};static bool multi;
static GLuint bound[16];
static bool has_feature(int f){(void)f;return multi;}
static void glBindVertexArray(GLuint id){assert(id==1);}
static void glBindVertexBuffer(GLuint i,GLuint b,GLintptr o,GLsizei s){assert(i<16);if(!b)assert(!o&&!s);bound[i]=b;}
static void glBindVertexBuffers(GLuint first,GLsizei n,const GLuint *b,const GLintptr *o,const GLsizei *s){for(int i=0;i<n;i++)glBindVertexBuffer(first+i,b[i],o[i],s[i]);}
static void vrend_resource_reference(struct vrend_resource **dst,struct vrend_resource *src){*dst=src;}
'''
main = r'''
int main(void){
 struct vrend_resource resources[3]={{1},{2},{3}};
 struct vrend_vertex_element_array va={1};
 for(unsigned path=0;path<2;path++){
  multi=path;memset(bound,0,sizeof(bound));
  struct vrend_sub_context sub={.num_vbos=3,.old_num_vbos=3,.vbo_dirty=true};
  struct vrend_context ctx={&sub};
  for(unsigned i=0;i<3;i++)sub.vbo[i]=(struct vrend_vertex_buffer){.base={&resources[i],0,16},.res_id=i+1};
  vrend_draw_bind_vertex_binding(&ctx,&va);assert(bound[0]==1&&bound[1]==2&&bound[2]==3&&!sub.vbo_dirty);
  /* No intermediate draw: remembering only the most recent count loses
   * the third GL binding, despite having dropped its resource reference. */
  vrend_set_num_vbo_sub(&sub,2);vrend_set_num_vbo_sub(&sub,1);
  assert(!sub.vbo[1].base.buffer&&!sub.vbo[2].base.buffer);
  vrend_draw_bind_vertex_binding(&ctx,&va);
  assert(bound[0]==1&&!bound[1]&&!bound[2]&&sub.old_num_vbos==1&&!sub.vbo_dirty);
  /* Repeated empty and restored bindings must not forget retired state. */
  vrend_set_num_vbo_sub(&sub,0);vrend_set_num_vbo_sub(&sub,0);
  sub.vbo[0]=(struct vrend_vertex_buffer){.base={&resources[2],8,32},.res_id=3};
  vrend_set_num_vbo_sub(&sub,1);vrend_draw_bind_vertex_binding(&ctx,&va);
  assert(bound[0]==3&&!bound[1]&&!bound[2]);
  vrend_set_num_vbo_sub(&sub,0);vrend_draw_bind_vertex_binding(&ctx,&va);
  assert(!bound[0]&&!bound[1]&&!bound[2]&&!sub.old_num_vbos);
 }
 puts("PASS: multiple VBO shrinks before drawing clear retired GL bindings on multi-bind and single-bind paths; zero/restore updates preserve state");
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-binding-state-') as tmp:
    p = Path(tmp)
    code = pre+setter+bind+main
    (p/'test.c').write_text(code)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(p/'test.c'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
    mutant = code.replace('MAX2(sub->old_num_vbos, old_num)', 'old_num')
    assert mutant != code
    (p/'mutant.c').write_text(mutant)
    subprocess.run(['cc', '-O2', str(p/'mutant.c'), '-o', str(p/'mutant')], check=True)
    assert subprocess.run([str(p/'mutant')], capture_output=True).returncode != 0
    print('PASS: preceding binding-count implementation fails the stale-binding regression')
