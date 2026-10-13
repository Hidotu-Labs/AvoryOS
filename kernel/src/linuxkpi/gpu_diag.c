/* Bounded native GPU flight recorder. Normal producer paths do not allocate
 * or print; failed shared-buffer setup prints at most 16 short boot records.
 * Checksums are explicitly armed and bounded to 64 copies. */
#include <linuxkpi/gpu_diag.h>
#include "lock/spinlock.h"
#include "sched/sched.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "lib/string.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "drivers/serial.h"

#define GPU_DIAG_CAPACITY 2048
struct gpu_diag_entry {
  uint64_t seq, ms, object, a, b, c, d;
  long result;
  unsigned int kind, tid;
};
static struct gpu_diag_entry entries[GPU_DIAG_CAPACITY];
static struct gpu_diag_entry anomalies[32];
static struct gpu_diag_entry pixel_checks[64];
static spinlock_t diag_lock = {0};
static unsigned int level = 1, samples;
static unsigned int prime_error_lines;
static uint64_t sequence, seen[GPU_DIAG_CACHE_CHECK + 1], changed, mismatch;
static uint64_t cache_bad, anomaly_count;
static const char *const names[] = {"?", "IOCTL", "CS_IN", "CS_OUT", "CACHE",
  "MAP", "FAULT", "IMPORT", "CPU_BEGIN", "CPU_END", "FENCE", "COPY_LAYOUT",
  "COPY_CHECK", "PRESENT", "CACHE_CHECK"};

unsigned int gpu_diag_level(void) { return __atomic_load_n(&level, __ATOMIC_RELAXED); }
void gpu_diag_record(unsigned int kind, const void *object, uint64_t a, uint64_t b,
                     uint64_t c, uint64_t d, long result) {
  if (!gpu_diag_level() || !kind || kind > GPU_DIAG_CACHE_CHECK) return;
  struct thread *t = sched_get_current();
  struct gpu_diag_entry e = {.ms=lapic_timer_get_ms(), .object=(uintptr_t)object,
    .a=a, .b=b, .c=c, .d=d, .result=result, .kind=kind, .tid=t ? t->tid : 0};
  spinlock_acquire(&diag_lock);
  /* A freeze racing this producer must remain a freeze. */
  if (level) {
    e.seq = ++sequence;
    entries[(e.seq-1) % GPU_DIAG_CAPACITY] = e;
    seen[kind]++;
    if (kind == GPU_DIAG_COPY_CHECK) {
      pixel_checks[(seen[kind]-1)%64] = e;
      changed += !!(result & 1);
      mismatch += !!(result & 2);
    }
    if (kind == GPU_DIAG_CACHE_CHECK && result) cache_bad++;
    if ((kind==GPU_DIAG_COPY_CHECK && result) ||
        (kind==GPU_DIAG_CACHE_CHECK && result) ||
        ((kind==GPU_DIAG_IMPORT || kind==GPU_DIAG_CPU_BEGIN || kind==GPU_DIAG_CPU_END || kind==GPU_DIAG_PRESENT) && result<0))
      anomalies[anomaly_count++%32]=e;
  }
  spinlock_release(&diag_lock);
}
void gpu_diag_prime_error(unsigned int stage, const void *object,
                          uint64_t detail, uint64_t size, long result) {
  if (result >= 0 || !gpu_diag_level())
    return;
  gpu_diag_record(GPU_DIAG_IMPORT, object, detail, size, stage, 0, result);
  /* Startup failures may leave users unable to reach a console or export
   * /proc/gpu_debug. Preserve the first errors directly in the host log.
   * These callers are setup ioctls in task context with no native KMS lock. */
  if (__atomic_fetch_add(&prime_error_lines, 1, __ATOMIC_RELAXED) < 16) {
    struct thread *t = sched_get_current();
    klogf("[GPUDBG] PRIME stage=%u detail=0x%llx size=%llu error=%ld tid=%u\n",
          stage, (unsigned long long)detail, (unsigned long long)size,
          result, t ? t->tid : 0);
  }
}
int gpu_diag_take_sample(void) {
  if (gpu_diag_level() != 2) return 0;
  unsigned int left = __atomic_load_n(&samples, __ATOMIC_RELAXED);
  while (left)
    if (__atomic_compare_exchange_n(&samples, &left, left-1, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return 1;
  return 0;
}
uint64_t gpu_diag_hash_rect(const void *pixels, size_t row, unsigned int height, size_t pitch) {
  const unsigned char *p = pixels;
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned int y=0; y<height; y++)
    for (size_t x=0; x<row; x++) hash = (hash ^ p[(size_t)y*pitch+x]) * 1099511628211ULL;
  return hash;
}
static void emit(unsigned int offset, unsigned int size, unsigned char *out,
                  uint64_t *position, unsigned int *copied, const char *line) {
  size_t len = strlen(line);
  if (*position + len > offset && *copied < size) {
    size_t skip = offset > *position ? offset-*position : 0;
    size_t n = len-skip;
    if (n > size-*copied) n = size-*copied;
    memcpy(out+*copied, line+skip, n); *copied += n;
  }
  *position += len;
}
unsigned int gpu_diag_read(unsigned int offset, unsigned int size, unsigned char *buffer) {
  if (!size || !buffer) return 0;
  char line[280]; uint64_t position=0, end, counts[GPU_DIAG_CACHE_CHECK+1], unstable, bad;
  unsigned int copied=0, mode, remaining;
  uint64_t wrong_cache, pinned;
  spinlock_acquire(&diag_lock);
  end=sequence; mode=level; remaining=__atomic_load_n(&samples,__ATOMIC_RELAXED); unstable=changed; bad=mismatch;
  memcpy(counts, seen, sizeof counts);
  wrong_cache=cache_bad; pinned=anomaly_count;
  spinlock_release(&diag_lock);
  snprintf(line,sizeof line,"GPUDBG v1 mode=%u samples_left=%u events=%llu overwritten=%llu source_changed=%llu stable_copy_mismatch=%llu cache_mismatch=%llu anomalies=%llu\n",
    mode,remaining,(unsigned long long)end,(unsigned long long)(end>GPU_DIAG_CAPACITY?end-GPU_DIAG_CAPACITY:0),
    (unsigned long long)unstable,(unsigned long long)bad,(unsigned long long)wrong_cache,(unsigned long long)pinned);
  emit(offset,size,buffer,&position,&copied,line);
  emit(offset,size,buffer,&position,&copied,"commands: 1=reset metadata; 2=reset metadata+64 pixel checks; freeze=retain; dump=freeze+serial; 0=stop; clear=reset stopped\n");
  for (unsigned int k=1; k<=GPU_DIAG_CACHE_CHECK; k++) {
    snprintf(line,sizeof line,"count %s=%llu\n",names[k],(unsigned long long)counts[k]);
    emit(offset,size,buffer,&position,&copied,line);
  }
  emit(offset,size,buffer,&position,&copied,"seq ms tid event object a b c d result (hex fields; result decimal)\n");
  /* The 64 expensive checks occur at the start of a capture. Routine ioctl
   * traffic otherwise overwrites all of them before the user freezes it. */
  emit(offset,size,buffer,&position,&copied,"retained pixel checks (last 64):\n");
  uint64_t check_end=counts[GPU_DIAG_COPY_CHECK];
  uint64_t check_first=check_end>64?check_end-64:0;
  for (uint64_t i=check_first; i<check_end && copied<size; i++) {
    struct gpu_diag_entry e;
    spinlock_acquire(&diag_lock);e=pixel_checks[i%64];spinlock_release(&diag_lock);
    snprintf(line,sizeof line,"sample %llu %llu %u COPY_CHECK %llx %llx %llx %llx %llx %ld\n",
      (unsigned long long)e.seq,(unsigned long long)e.ms,e.tid,
      (unsigned long long)e.object,(unsigned long long)e.a,(unsigned long long)e.b,
      (unsigned long long)e.c,(unsigned long long)e.d,e.result);
    emit(offset,size,buffer,&position,&copied,line);
  }
  uint64_t first=end>GPU_DIAG_CAPACITY?end-GPU_DIAG_CAPACITY+1:1;
  for (uint64_t seq=first; seq<=end && copied<size; seq++) {
    struct gpu_diag_entry e;
    spinlock_acquire(&diag_lock); e=entries[(seq-1)%GPU_DIAG_CAPACITY]; spinlock_release(&diag_lock);
    if (e.seq != seq) snprintf(line,sizeof line,"event %llu overwritten while reading; freeze first\n",(unsigned long long)seq);
    else snprintf(line,sizeof line,"%llu %llu %u %s %llx %llx %llx %llx %llx %ld\n",
      (unsigned long long)e.seq,(unsigned long long)e.ms,e.tid,names[e.kind],
      (unsigned long long)e.object,(unsigned long long)e.a,(unsigned long long)e.b,
      (unsigned long long)e.c,(unsigned long long)e.d,e.result);
    emit(offset,size,buffer,&position,&copied,line);
  }
  emit(offset,size,buffer,&position,&copied,"pinned anomalies (last 32; source changes can also be concurrent CPU writes):\n");
  first=pinned>32?pinned-32:0;
  for (uint64_t i=first; i<pinned && copied<size; i++) {
    struct gpu_diag_entry e;
    spinlock_acquire(&diag_lock);e=anomalies[i%32];spinlock_release(&diag_lock);
    snprintf(line,sizeof line,"anomaly %llu %llu %u %s %llx %llx %llx %llx %llx %ld\n",
      (unsigned long long)e.seq,(unsigned long long)e.ms,e.tid,names[e.kind],
      (unsigned long long)e.object,(unsigned long long)e.a,(unsigned long long)e.b,
      (unsigned long long)e.c,(unsigned long long)e.d,e.result);
    emit(offset,size,buffer,&position,&copied,line);
  }
  return copied;
}
static bool gpu_diag_drain_serial(unsigned int limit, uint64_t started) {
  while (serial_pending_bytes()>limit) {
    serial_flush_idle();
    if (lapic_timer_get_ms()-started>=30000) return false;
    lapic_timer_sleep(1);
  }
  return true;
}
void gpu_diag_dump(void) {
  spinlock_acquire(&diag_lock); __atomic_store_n(&level,0,__ATOMIC_RELAXED); spinlock_release(&diag_lock);
  char text[513]; unsigned int offset=0, n;
  uint64_t started=lapic_timer_get_ms();
  klog_puts("[GPUDBG] frozen flight recorder begin\n");
  while ((n=gpu_diag_read(offset,512,(unsigned char *)text))) {
    /* Queue complete lines so another CPU's log record cannot be inserted
     * into an event whose tail was left for the next 512-byte chunk. */
    while (n && text[n-1]!='\n') n--;
    if (!n) return; /* All formatted lines fit in this chunk. */
    /* klog is a best-effort async mirror. A full report exceeds the serial
     * ring, so pacing only this explicit process-context dump keeps it from
     * silently losing the middle and end. Never wait with diag_lock held.
     * A disconnected/stalled UART must not keep a proc write blocked forever;
     * the frozen proc file remains readable if this export times out. */
    if (!gpu_diag_drain_serial(8192,started)) {
      klog_puts("[GPUDBG] serial export timed out; read frozen /proc/gpu_debug\n");
      return;
    }
    text[n]=0; klog_puts(text); offset+=n;
  }
  klog_puts("[GPUDBG] frozen flight recorder end\n");
  gpu_diag_drain_serial(0,started);
}
unsigned int gpu_diag_control(unsigned int size, const unsigned char *buffer) {
  if (!size || !buffer || size>32) return 0;
  unsigned int original_size=size;
  char cmd[33]; memcpy(cmd,buffer,size); cmd[size]=0;
  while (size && (cmd[size-1]=='\n'||cmd[size-1]==' ')) cmd[--size]=0;
  bool reset=false; unsigned int next=0;
  if (!strcmp(cmd,"dump")) { gpu_diag_dump(); return original_size; }
  if (!strcmp(cmd,"1")) { next=1; reset=true; }
  else if (!strcmp(cmd,"2")) { next=2; reset=true; }
  else if (!strcmp(cmd,"clear")) reset=true;
  else if (strcmp(cmd,"0") && strcmp(cmd,"freeze")) return 0;
  spinlock_acquire(&diag_lock);
  if (reset) { sequence=changed=mismatch=cache_bad=anomaly_count=0; memset(entries,0,sizeof entries); memset(seen,0,sizeof seen); memset(anomalies,0,sizeof anomalies); memset(pixel_checks,0,sizeof pixel_checks); }
  __atomic_store_n(&samples,next==2?64:0,__ATOMIC_RELAXED);
  __atomic_store_n(&level,next,__ATOMIC_RELAXED);
  spinlock_release(&diag_lock);
  klog_puts(next==2?"[GPUDBG] armed 64 pixel checks (changes timing)\n":
            next==1?"[GPUDBG] recording metadata\n":"[GPUDBG] recording frozen/stopped\n");
  return original_size;
}

void gpu_diag_cache_check(const void *object, uint64_t physical, uint64_t expected) {
  if (!gpu_diag_level()) return;
  uint64_t actual=vmm_debug_cache_flags(vmm_get_kernel_pml4(),physical+pmm_get_hhdm_offset());
  gpu_diag_record(GPU_DIAG_CACHE_CHECK,object,physical,expected,actual,0,
                  actual==UINT64_MAX ? -1 : (actual!=expected));
}
