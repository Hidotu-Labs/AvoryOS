#!/usr/bin/env python3
"""Exercise imported DRM FIFO/hole-tree callers against the real native rb_*.

Linux inline tree code and node layouts are used verbatim; only unrelated
kernel header dependencies and the callers' already-held locks are shimmed.
The old rotation behavior is also built and must fail the same invariants.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


sched = (ROOT / 'kernel/linux/drivers/gpu/drm/scheduler/sched_main.c').read_text()
mm = (ROOT / 'kernel/linux/drivers/gpu/drm/drm_mm.c').read_text()
linux_header = (ROOT / 'kernel/linux/include/linux/rbtree.h').read_text()
linux_header = re.sub(r'^#include.*\n', '', linux_header, flags=re.M)
linux_types = (ROOT / 'kernel/linux/include/linux/rbtree_types.h').read_text()
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
#define READ_ONCE(x) (x)
#define rcu_assign_pointer(p,v) ((p)=(v))
'''
callers = r'''
typedef int64_t ktime_t;
typedef uint64_t u64;
struct drm_sched_rq { struct rb_root_cached rb_tree_root; unsigned lock; };
struct drm_sched_entity { struct rb_node rb_tree_node; struct drm_sched_rq *rq;
    unsigned rq_lock; ktime_t oldest_job_waiting; };
struct drm_mm_node { struct rb_node rb_hole_size; u64 hole_size; };
#define spin_lock(p) ((void)(p))
#define spin_unlock(p) ((void)(p))
#define ktime_before(a,b) ((a)<(b))
'''
callers += function(sched, 'static __always_inline bool drm_sched_entity_compare_before(')
callers += function(sched, 'static inline void drm_sched_rq_remove_fifo_locked(')
callers += function(sched, 'void drm_sched_rq_update_fifo(')
callers += function(mm, 'static u64 rb_to_hole_size(')
callers += function(mm, 'static void insert_hole_size(')
# Exercise the exact in-kernel guard through the same Linux header view.
boot = (ROOT/'kernel/src/tests/linuxkpi/linux/test_phase1_libs.c').read_text()
boot = boot[boot.index('struct rb_test_elem {'):boot.index('void linuxkpi_test_phase1_libs(void)')]
callers += '\n#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))\n' + boot
main = r'''
#define N 257
static struct drm_sched_entity entities[N];
static struct drm_mm_node holes[N];
static bool live[N],visited[N];
static unsigned ops;
static uint64_t rng=0x91cbd2499ef263a7ULL;
static uint32_t random32(void) {rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng>>16;}
static unsigned node_index(const struct rb_node *n,bool hole) {
 uintptr_t base=(uintptr_t)(hole?(void*)holes:(void*)entities);
 size_t size=hole?sizeof(holes[0]):sizeof(entities[0]);
 size_t off=hole?offsetof(struct drm_mm_node,rb_hole_size):offsetof(struct drm_sched_entity,rb_tree_node);
 uintptr_t addr=(uintptr_t)n;
 assert(addr>=base+off && (addr-base-off)%size==0);
 unsigned i=(addr-base-off)/size;assert(i<N);return i;
}
static int64_t key(const struct rb_node*n,bool hole){
 return hole?-(int64_t)rb_entry(n,struct drm_mm_node,rb_hole_size)->hole_size:
 rb_entry(n,struct drm_sched_entity,rb_tree_node)->oldest_job_waiting;
}
/* Independent oracle: node membership, parent symmetry, total order, red
 * adjacency, equal black heights and a bounded walk (cycles cannot hang). */
static unsigned check_node(struct rb_node*n,struct rb_node*parent,bool hole,
                           int64_t low,int64_t high,unsigned depth){
 if(!n)return 1;
 assert(depth<N && rb_parent(n)==parent);
 unsigned i=node_index(n,hole);assert(live[i]&&!visited[i]);visited[i]=true;
 int64_t k=key(n,hole);assert(k>=low&&k<=high);
 bool black=n->__rb_parent_color&1;
 if(!black){assert(!n->rb_left||(n->rb_left->__rb_parent_color&1));assert(!n->rb_right||(n->rb_right->__rb_parent_color&1));}
 unsigned l=check_node(n->rb_left,n,hole,low,k,depth+1);
 unsigned r=check_node(n->rb_right,n,hole,k,high,depth+1);
 assert(l==r);return l+black;
}
static void check(struct rb_root_cached *root,bool hole){
 memset(visited,0,sizeof visited);
 if(root->rb_root.rb_node)assert(root->rb_root.rb_node->__rb_parent_color&1);
 check_node(root->rb_root.rb_node,NULL,hole,INT64_MIN,INT64_MAX,0);
 unsigned expected=0;
 for(unsigned i=0;i<N;i++){assert(live[i]==visited[i]);expected+=live[i];}
 assert(rb_first_cached(root)==rb_first(&root->rb_root));
 unsigned n=0;int64_t prev=INT64_MIN;
 for(struct rb_node*p=rb_first_cached(root);p;p=rb_next(p)){
  assert(n++<N&&key(p,hole)>=prev);prev=key(p,hole);
 }assert(n==expected);
 n=0;prev=INT64_MAX;
 for(struct rb_node*p=rb_last(&root->rb_root);p;p=rb_prev(p)){
  assert(n++<N&&key(p,hole)<=prev);prev=key(p,hole);
 }assert(n==expected);ops++;
}
static void fifo(void){
 struct drm_sched_rq rq={.rb_tree_root=RB_ROOT_CACHED};
 memset(live,0,sizeof live);
 for(unsigned i=0;i<N;i++){entities[i].rq=&rq;RB_CLEAR_NODE(&entities[i].rb_tree_node);}
 for(unsigned pass=0;pass<3;pass++){
  for(unsigned i=0;i<N;i++){
   ktime_t ts=pass==0?i:pass==1?N-i:random32()%64;
   drm_sched_rq_update_fifo(&entities[i],ts);live[i]=true;check(&rq.rb_tree_root,false);
  }
  for(unsigned k=0;k<20000;k++){
   unsigned i=random32()%N;
   if(random32()&1){drm_sched_rq_update_fifo(&entities[i],random32()%512);live[i]=true;}
   else {drm_sched_rq_remove_fifo_locked(&entities[i]);live[i]=false;}
   check(&rq.rb_tree_root,false);
  }
  /* Removing the current root exercises direct/deep successor replacement. */
  while(rq.rb_tree_root.rb_root.rb_node){
   unsigned i=node_index(rq.rb_tree_root.rb_root.rb_node,false);
   drm_sched_rq_remove_fifo_locked(&entities[i]);live[i]=false;check(&rq.rb_tree_root,false);
  }
 }
}
static void hole_tree(void){
 struct rb_root_cached root=RB_ROOT_CACHED;memset(live,0,sizeof live);
 for(unsigned i=0;i<N;i++){holes[i].hole_size=i+1;insert_hole_size(&root,&holes[i]);live[i]=true;check(&root,true);}
 for(unsigned k=0;k<20000;k++){
  unsigned i=random32()%N;
  rb_erase_cached(&holes[i].rb_hole_size,&root);live[i]=false;check(&root,true);
  holes[i].hole_size=random32()%512;insert_hole_size(&root,&holes[i]);live[i]=true;check(&root,true);
 }
 while(root.rb_root.rb_node){
  unsigned i=node_index(root.rb_root.rb_node,true);
  rb_erase_cached(&holes[i].rb_hole_size,&root);live[i]=false;check(&root,true);
 }
}
int main(void){assert(test_rbtree_native_bridge());fifo();hole_tree();printf("PASS: %u DRM FIFO/hole-tree operations with exact membership, ordering, colors, cached minima and parent links\n",ops);}
'''

native = (ROOT / 'kernel/src/lib/rbtree.c').read_text()
old = native.replace('rb_set_parent(right, parent);',
                     'rb_set_parent_color(right, parent, rb_color(node));')
old = old.replace('rb_set_parent(node, right);', 'rb_set_parent_color(node, right, RB_RED);')
old = old.replace('rb_set_parent(left, parent);',
                  'rb_set_parent_color(left, parent, rb_color(node));')
old = old.replace('rb_set_parent(node, left);', 'rb_set_parent_color(node, left, RB_RED);')
assert old != native

with tempfile.TemporaryDirectory(prefix='avory-kpi-rbtree-') as tmp:
    p = Path(tmp)
    (p / 'drm.c').write_text(shim + linux_types + linux_header + callers + main)
    for name, source in [('fixed', native), ('old', old)]:
        (p / 'native.c').write_text(source)
        exe = p / name
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I'+str(ROOT/'kernel/src/lib'), str(p/'native.c'), str(p/'drm.c'),
                        '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        if name == 'fixed':
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout, end='')
        else:
            assert result.returncode != 0 and 'Assertion' in result.stderr, result.stdout + result.stderr
            print('PASS: original color-changing rotations fail the same DRM caller regression')

    # Also exercise the native scheduler's existing augmented-tree self-test.
    original = (ROOT/'kernel/src/tests/sched/test_phase1_rbtree.c').read_text()
    end = original.index(function(original, 'static void test_rb_augmented(void)'))
    phase = original[:end] + function(original, 'static void test_rb_augmented(void)')
    phase = re.sub(r'^#include.*\n', '', phase, flags=re.M)
    (p/'phase.c').write_text('#include "rbtree.h"\n#include <assert.h>\n#include <stdio.h>\n'
                            'static void klog_puts(const char*s){fputs(s,stdout);}\n' + phase +
                            '\nint main(void){test_rb_basic();test_rb_augmented();assert(!g_phase1_fail);puts("PASS: native scheduler basic and augmented-tree self-tests");}\n')
    (p/'native.c').write_text(native)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-I'+str(ROOT/'kernel/src/lib'), str(p/'native.c'), str(p/'phase.c'),
                    '-o', str(p/'phase')], check=True)
    subprocess.run([str(p/'phase')], check=True, timeout=30)
