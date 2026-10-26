/* cc -O2 -ffunction-sections -fdata-sections -Ikernel/src
 * -Ikernel/linuxkpi/include -Ikernel/limine-protocol/include
 * scripts/tests/test_evdev_readers.c -Wl,--gc-sections -o /tmp/test-evdev-readers */
#define sched_yield evdev_test_yield
#include "../../kernel/src/drivers/input/evdev.c"
extern void *calloc(size_t, size_t);
extern void free(void *);
extern int puts(const char *);
extern int dprintf(int, const char *, ...);
extern void abort(void);
#define CHECK(x) do { if (!(x)) { dprintf(2,"FAIL: %s\n", #x); abort(); } } while (0)
static struct thread task;
static vfs_node_t *notified[16];
static unsigned notifications;
void *kmalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
void vfs_node_init(vfs_node_t *n) { memset(n,0,sizeof(*n)); }
struct thread *sched_get_current(void) { return &task; }
void sched_yield(void) { abort(); } /* Tests never wait for absent input. */
void wait_queue_add(wait_queue_t *q,wait_queue_entry_t *e) { (void)q;(void)e; }
void wait_queue_remove(wait_queue_t *q,wait_queue_entry_t *e) { (void)q;(void)e; }
void wait_queue_wake_all(wait_queue_t *q) { (void)q; }
void epoll_notify_event(vfs_node_t *n,uint32_t events) {
 CHECK(events==POLLIN);
 if (notifications<16) notified[notifications++]=n;
}
hal_irq_state_t hal_irq_save(void) { return 0; }
void hal_irq_restore(hal_irq_state_t f) { (void)f; }
void tlb_shootdown_poll(void) {}
void vfs_rcu_poll(void) {}
void lockdiag_spin_begin(const void *p,uint64_t ip) { (void)p;(void)ip; }
void lockdiag_spin_end(const void *p) { (void)p; }
void lockdiag_keyboard_event(uint16_t c,bool pressed) { (void)c;(void)pressed; }
uint64_t pit_get_ticks(void) { return 1234; }
bool mouse_has_absolute_axes(void) { return true; }
void klog_puts(const char *s) { (void)s; }
void klog_uint64(uint64_t n) { (void)n; }
void klog_hex32(uint32_t n) { (void)n; }
int main(void) {
 evdev_device_t dev={0};vfs_node_t metadata={0};
 metadata.device=&dev;metadata.read=evdev_vfs_read;metadata.poll=evdev_vfs_poll;
 metadata.write=evdev_vfs_write;metadata.ioctl=evdev_vfs_ioctl;
 strcpy(metadata.name,"event1");metadata.mask=0666;metadata.inode=(13<<8)|65;
 dev.vfs_node=&metadata;
 vfs_node_t *xorg=evdev_open_instance(&metadata),*wayland=evdev_open_instance(&metadata);
 CHECK(xorg && wayland && xorg!=wayland && xorg->inode==wayland->inode);
 xorg->flags|=FS_NONBLOCK;wayland->flags|=FS_NONBLOCK;
 struct input_event a[EVDEV_RING_SIZE+1],b[EVDEV_RING_SIZE+1];
 evdev_push_event(&dev,EV_REL,REL_X,17);evdev_sync(&dev);
 CHECK(xorg->poll(xorg,POLLIN)&POLLIN);CHECK(wayland->poll(wayland,POLLIN)&POLLIN);
 CHECK(notified[0]!=notified[1]);
 CHECK(xorg->read(xorg,0,sizeof(a),(uint8_t*)a)==2*sizeof(*a));
 CHECK(wayland->poll(wayland,POLLIN)&POLLIN);
 CHECK(wayland->read(wayland,0,sizeof(b),(uint8_t*)b)==2*sizeof(*b));
 CHECK(!memcmp(a,b,2*sizeof(*a)));
 CHECK(xorg->read(xorg,0,sizeof(a),(uint8_t*)a)==(uint32_t)-11);
 CHECK(wayland->read(wayland,0,sizeof(b),(uint8_t*)b)==(uint32_t)-11);
 evdev_report_key(&dev,BTN_LEFT,true);
 evdev_push_event(&dev,EV_REL,REL_WHEEL,-1);
 evdev_report_key(&dev,BTN_LEFT,false);evdev_sync(&dev);
 CHECK(xorg->read(xorg,0,sizeof(a),(uint8_t*)a)==4*sizeof(*a));
 CHECK(wayland->read(wayland,0,sizeof(b),(uint8_t*)b)==4*sizeof(*b));
 CHECK(!memcmp(a,b,4*sizeof(*a)));
 CHECK(a[0].code==BTN_LEFT && a[0].value==1);
 CHECK(a[1].code==REL_WHEEL && a[1].value==-1);
 CHECK(a[2].code==BTN_LEFT && a[2].value==0);
 /* New opens do not replay old events. Sharing one node (dup/fork) shares a cursor. */
 evdev_push_event(&dev,EV_REL,REL_Y,9);evdev_sync(&dev);
 vfs_node_t *late=evdev_open_instance(&metadata);late->flags|=FS_NONBLOCK;
 CHECK(!late->poll(late,POLLIN));
 CHECK(xorg->read(xorg,0,sizeof(a),(uint8_t*)a)==2*sizeof(*a));
 CHECK(!xorg->poll(xorg,POLLIN));
 CHECK(wayland->read(wayland,0,sizeof(b),(uint8_t*)b)==2*sizeof(*b));
 /* A slow reader overflows independently; the active reader loses nothing. */
 for (unsigned i=0;i<EVDEV_RING_SIZE+8;i++) {
  evdev_push_event(&dev,EV_REL,REL_X,i);evdev_sync(&dev);
  CHECK(wayland->read(wayland,0,sizeof(b),(uint8_t*)b)==2*sizeof(*b));
  CHECK(b[0].type==EV_REL && b[0].value==(int)i);
 }
 CHECK(xorg->read(xorg,0,sizeof(a),(uint8_t*)a)>0);
 CHECK(a[0].type==EV_SYN && a[0].code==SYN_DROPPED);
 late->close(late);free(late);xorg->close(xorg);free(xorg);
 evdev_push_event(&dev,EV_REL,REL_X,99);evdev_sync(&dev);
 CHECK(wayland->read(wayland,0,sizeof(b),(uint8_t*)b)==2*sizeof(*b));
 wayland->close(wayland);free(wayland);CHECK(!dev.clients);
 puts("PASS: independent evdev readers, readiness, late opens, overflow and final close");
}
