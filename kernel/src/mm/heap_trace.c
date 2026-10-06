#include "mm/heap_internal.h"
#include "console/console.h"

/* Bounded log of recent frees: (object, caller).  This is only a debugging
 * aid: use-after-free reports name the call site that handed the object back
 * to the heap, which usually identifies the double owner.  Plain 64-bit
 * stores keep it lock-free; a racing read can at worst miss an entry. */
#define KFREE_TRACE_DEPTH 256
static struct {
  void *ptr;
  void *caller;
} kfree_trace[KFREE_TRACE_DEPTH];
static uint32_t kfree_trace_next;

void heap_trace_record(void *ptr, void *caller) {
  uint32_t slot =
      __atomic_add_fetch(&kfree_trace_next, 1, __ATOMIC_RELAXED) %
      KFREE_TRACE_DEPTH;
  kfree_trace[slot].ptr = ptr;
  __atomic_store_n(&kfree_trace[slot].caller, caller, __ATOMIC_RELEASE);
}

/* Allocation-free hex printer for use inside the allocator itself. */
static void kfree_put_hex64(uint64_t v) {
  static const char digits[] = "0123456789abcdef";
  console_puts("0x");
  for (int i = 60; i >= 0; i -= 4)
    console_putchar(digits[(v >> i) & 0xF]);
}

/* Double-free report: names the object, the caller attempting the second
 * free (newest trace entry - kfree records its caller on entry), and the
 * caller that performed the first free (second-newest entry).  Feed both PCs
 * to addr2line against kernel/bin-x86_64/kernel to find the double owner. */
void heap_trace_warn_double(const char *where, void *ptr) {
  uint64_t caller = 0, prev = 0;
  int found = 0;
  for (uint32_t n = 1; n <= KFREE_TRACE_DEPTH; n++) {
    uint32_t slot = (kfree_trace_next - n) % KFREE_TRACE_DEPTH;
    if (__atomic_load_n(&kfree_trace[slot].ptr, __ATOMIC_ACQUIRE) == ptr &&
        kfree_trace[slot].caller) {
      if (found == 0)
        caller = (uint64_t)kfree_trace[slot].caller;
      else {
        prev = (uint64_t)kfree_trace[slot].caller;
        break;
      }
      found++;
    }
  }
  console_puts("[WARN] kfree: double free intercepted ");
  console_puts(where);
  console_puts(" obj=");
  kfree_put_hex64((uint64_t)ptr);
  if (caller) {
    console_puts(" caller=");
    kfree_put_hex64(caller);
  }
  if (prev) {
    console_puts(" prev_free=");
    kfree_put_hex64(prev);
  }
  console_puts("\n");
}

bool heap_last_free_caller(const void *ptr, uint64_t *caller) {
  if (!ptr || !caller)
    return false;
  for (uint32_t n = 1; n <= KFREE_TRACE_DEPTH; n++) {
    uint32_t slot = (kfree_trace_next - n) % KFREE_TRACE_DEPTH;
    if (__atomic_load_n(&kfree_trace[slot].ptr, __ATOMIC_ACQUIRE) == ptr &&
        kfree_trace[slot].caller) {
      *caller = (uint64_t)kfree_trace[slot].caller;
      return true;
    }
  }
  return false;
}
