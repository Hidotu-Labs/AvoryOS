#!/usr/bin/env python3
"""Exercise actual KPI poll subscriptions and wake paths against close/free."""
from pathlib import Path
import ast
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
native=(root/'kernel/src/linuxkpi/native_vfs.c').read_text()
assert 'epoll_notify_event(node, 0xFFFFu);' in native
file_code=(root/'kernel/linuxkpi/src/file.c').read_text()
assert file_code.index('linuxkpi_poll_detach(&file->f_poll_links, NULL)') < file_code.index('file->f_op->release(inode, file)')
tree=ast.parse((root/'scripts/tests/test_kpi_completion.py').read_text())
pre=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='PRELUDE' for t in n.targets))
pre=pre.replace('static void asc_vfs_poll_notify(void *node,void *wq) {(void)node;(void)wq;}',r'''
struct test_node { bool live; unsigned epoll_wakes; };
struct test_queue { bool live; unsigned poll_wakes; };
static atomic_bool callback_entered, callback_release, close_done;
static bool hold_callback, fail_alloc;
static void asc_vfs_poll_notify(void *node,void *wq) {
 struct test_node *n=node;struct test_queue *q=wq;
 assert(n->live && q->live);n->epoll_wakes++;q->poll_wakes++;
 if(hold_callback) {
  atomic_store(&callback_entered,true);
  while(!atomic_load(&callback_release))sched_yield();
  assert(n->live && q->live);
 }
}
''')
pre=pre.replace('#include <stdio.h>', '#include <stdio.h>\n#include <stdatomic.h>')
# Allocation failure exercises qproc error reporting separately from readiness.
pre=pre.replace('return calloc(1,n);','return fail_alloc ? NULL : calloc(1,n);')
pre=pre.replace('static void *kzalloc', 'static bool fail_alloc;\nstatic void *kzalloc').replace('static bool hold_callback, fail_alloc;', 'static bool hold_callback;')
def stripped(path):
 return re.sub(r'^#include[^\n]*\n','',(root/path).read_text(),flags=re.M)
code=pre+stripped('kernel/linuxkpi/include/linux/wait.h')+stripped('kernel/linuxkpi/src/wait.c')
# Exercise the actual file poll/qproc adapter as well as the queue machinery.
file_source=(root/'kernel/linuxkpi/src/file.c').read_text()
a=file_source.index('struct kpi_poll_table {')
b=file_source.index('unsigned int linuxkpi_file_read(',a)
code+=r'''
#define POLLERR 8
#define POLLHUP 16
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
struct poll_table_struct { void (*_qproc)(struct file *,wait_queue_head_t *,struct poll_table_struct *); unsigned _key; };
struct file_operations { unsigned (*poll)(struct file *,struct poll_table_struct *); };
struct file { const struct file_operations *f_op; struct kpi_poll_link *f_poll_links;void *f_poll_wq; };
static struct file *poll_file;
static struct file *kpi_node_file(void *node) {(void)node;return poll_file;}
'''
# Forward declaration avoids a prototype-scope struct file in the poll table.
code=code.replace('struct poll_table_struct {','struct file;\nstruct poll_table_struct {')
code+='static unsigned int kpi_poll_mask_to_native(unsigned int mask) {return mask & 0xffff;}\n'
code+=file_source[a:b]
code+=r'''
static wait_queue_head_t shared;
static struct kpi_poll_link *links;
static struct test_node node={true,0};
static struct test_queue native={true,0};
static void *waker(void *p) {(void)p;wake_up(&shared);return NULL;}
static void *closer(void *p) {
 (void)p;linuxkpi_poll_detach(&links,&node);
 node.live=false;native.live=false;atomic_store(&close_done,true);return NULL;
}
static unsigned driver_poll(struct file *f,struct poll_table_struct *pt) {
 assert(pt->_key & 1);pt->_qproc(f,&shared,pt);return 0;
}
int main(void) {
 init_waitqueue_head(&shared);
 struct kpi_poll_link *other_links=NULL;
 struct test_node other={true,0};struct test_queue otherq={true,0};
 assert(!linuxkpi_poll_register(&links,&shared,&node,&native));
 assert(!linuxkpi_poll_register(&links,&shared,&node,&native));
 assert(!linuxkpi_poll_register(&other_links,&shared,&other,&otherq));
 wake_up(&shared);
 assert(node.epoll_wakes==1 && native.poll_wakes==1 && other.epoll_wakes==1);
 unsigned long flags;spin_lock_irqsave(&shared.lock,flags);
 wake_up_locked(&shared);spin_unlock_irqrestore(&shared.lock,flags);
 assert(node.epoll_wakes==2 && other.epoll_wakes==2);
 linuxkpi_poll_detach(&other_links,&other);other.live=false;otherq.live=false;
 wake_up(&shared);assert(node.epoll_wakes==3 && other.epoll_wakes==2);
 /* Multiple exported nodes of one file all receive notifications. */
 struct test_node exported={true,0};
 assert(!linuxkpi_poll_register(&links,&shared,&exported,&native));
 wake_up(&shared);assert(node.epoll_wakes==4 && exported.epoll_wakes==1);
 linuxkpi_poll_detach(&links,&exported);exported.live=false;
 /* An in-flight wake must finish before close can invalidate node/queue. */
 hold_callback=true;pthread_t wt,ct;
 assert(!pthread_create(&wt,NULL,waker,NULL));
 while(!atomic_load(&callback_entered))sched_yield();
 assert(!pthread_create(&ct,NULL,closer,NULL));
 struct timespec pause={0,20000000};nanosleep(&pause,NULL);
 assert(!atomic_load(&close_done));atomic_store(&callback_release,true);
 pthread_join(wt,NULL);pthread_join(ct,NULL);
 wake_up(&shared);assert(!links && !shared.kpi_poll_links);
 /* POLLFREE invalidates queue pointers before the head itself is freed. */
 wait_queue_head_t *dynamic=calloc(1,sizeof(*dynamic));init_waitqueue_head(dynamic);
 node.live=native.live=true;hold_callback=false;
 assert(!linuxkpi_poll_register(&links,dynamic,&node,&native));
 wake_up_pollfree(dynamic);free(dynamic);linuxkpi_poll_detach(&links,NULL);
 assert(!links);
 /* epoll's initial VFS poll registers even when the driver is not ready. */
 struct file_operations ops={driver_poll};
 struct file file={.f_op=&ops,.f_poll_wq=&native};poll_file=&file;
 unsigned before=node.epoll_wakes;
 assert(linuxkpi_file_poll(&node,1)==0);
 assert(linuxkpi_file_poll(&node,1)==0);
 wake_up(&shared);assert(node.epoll_wakes==before+1);
 linuxkpi_poll_detach(&file.f_poll_links,NULL);
 fail_alloc=true;
 assert(linuxkpi_file_poll(&node,1)==8 && !file.f_poll_links);
 assert(linuxkpi_poll_register(&links,&shared,&node,&native)==-ENOMEM && !links);
 puts("PASS: shared queues, duplicate registration, exported nodes, locked wakes, close/wake race and POLLFREE");
}
'''
with tempfile.TemporaryDirectory(prefix='kpi-poll-bridge-') as tmp:
 p=Path(tmp)/'check';p.with_suffix('.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O1','-g','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(p.with_suffix('.c')),'-o',str(p)],check=True)
 subprocess.run([str(p)],check=True,timeout=10)
