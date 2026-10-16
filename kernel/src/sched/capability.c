#include "capability.h"
#include "sched.h"
#include "sched_internal.h"
#include "pid_namespace.h"
#include "../include/arch/uaccess.h"
/* A new boot task starts privileged. Children always receive an explicit
 * credential snapshot, so dropping all capabilities never falls back to UID. */
static void ensure(struct thread *t) {
  if (!t || __atomic_load_n(&t->caps_initialized, __ATOMIC_ACQUIRE)) return;
  spinlock_acquire(&t->namespace_lock);
  if (t->caps_initialized) { spinlock_release(&t->namespace_lock); return; }
  t->cap_bounding = CAP_VALID_MASK;
  t->cap_effective = t->cap_permitted = t->euid == 0 ? CAP_VALID_MASK : 0;
  t->cap_inheritable = t->cap_ambient = 0;
  __atomic_store_n(&t->caps_initialized, true, __ATOMIC_RELEASE);
  spinlock_release(&t->namespace_lock);
}
bool capability_has(struct thread *t, unsigned cap) {
  if (!t) return true; /* Kernel callers. */
  ensure(t);
  return cap <= CAP_LAST_CAP && (t->cap_effective & (1ULL << cap));
}
void capability_inherit(struct thread *child, struct thread *parent) {
  ensure(parent);
  child->cap_effective = parent->cap_effective;
  child->cap_permitted = parent->cap_permitted;
  child->cap_inheritable = parent->cap_inheritable;
  child->cap_bounding = parent->cap_bounding;
  child->cap_ambient = parent->cap_ambient;
  child->securebits = parent->securebits;
  child->caps_initialized = true;

}
void capability_uid_changed(struct thread *t, uint32_t uid, uint32_t euid, uint32_t suid) {
  ensure(t);
  if (t->securebits & 4) return; /* SECBIT_NO_SETUID_FIXUP */
  if ((uid == 0 || euid == 0 || suid == 0) &&
      t->uid != 0 && t->euid != 0 && t->suid != 0) {
    if (!(t->securebits & 16)) t->cap_permitted = 0;
    t->cap_effective = t->cap_ambient = 0;
  } else if (euid == 0 && t->euid != 0) t->cap_effective = 0;
  else if (euid != 0 && t->euid == 0) t->cap_effective = t->cap_permitted;
}
void capability_exec(struct thread *t, uint32_t old_euid) {
  ensure(t);
  uint64_t p = t->cap_ambient;
  if (!(t->securebits & 1) && (t->uid == 0 || t->euid == 0))
    p |= t->cap_bounding | t->cap_inheritable;
  if (t->no_new_privs) p &= t->cap_permitted;
  if (old_euid != t->euid) t->cap_ambient = 0;
  t->cap_permitted = p;
  t->cap_effective = t->euid == 0 ? p : t->cap_ambient;
  t->securebits &= ~16u;
}
int64_t capability_prctl(struct thread *t, uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  if (!t) return -3;
  ensure(t);
  if (op == 7) return !!(t->securebits & 16);
  if (op == 8) {
    if (a > 1) return -22;
    if (t->securebits & 32) return -1;
    t->securebits = (t->securebits & ~16u) | (a ? 16 : 0); return 0;
  }
  if (op == 27) return t->securebits;
  if (op == 28) {
    if (a & ~255u) return -22;
    if (!capability_has(t, CAP_SETPCAP)) return -1;
    for (unsigned bit = 1; bit < 8; bit += 2) {
      unsigned lock = 1u << bit;
      if ((t->securebits & lock) &&
          ((a & (lock | (lock >> 1))) != (t->securebits & (lock | (lock >> 1))))) return -1;
    }
    t->securebits = a; return 0;
  }
  if (op == 23 || op == 24) {
    if (a > CAP_LAST_CAP) return -22;
    if (op == 23) return !!(t->cap_bounding & (1ULL << a));
    if (!capability_has(t, CAP_SETPCAP)) return -1;
    t->cap_bounding &= ~(1ULL << a); return 0;
  }
  if (op == 47) {
    if (c || d) return -22;
    if (a == 4) { if (b) return -22; t->cap_ambient = 0; return 0; }
    if (b > CAP_LAST_CAP) return -22;
    uint64_t bit = 1ULL << b;
    if (a == 1) return !!(t->cap_ambient & bit);
    if (a == 2) {
      if ((t->securebits & 64) || !(t->cap_permitted & bit) || !(t->cap_inheritable & bit)) return -1;
      t->cap_ambient |= bit; return 0;
    }
    if (a == 3) { t->cap_ambient &= ~bit; return 0; }
  }
  return -22;
}
struct cap_header { uint32_t version; int32_t pid; };
struct cap_data { uint32_t effective, permitted, inheritable; };
static int words(uint64_t h, uint64_t data, struct cap_header *hdr) {
  if (copy_from_user(hdr, (void *)h, sizeof(*hdr))) return -14;
  if (hdr->version == 0x19980330) return 1;
  if (hdr->version == 0x20071026 || hdr->version == 0x20080522) return 2;
  uint32_t version = 0x20080522;
  if (copy_to_user((void *)h, &version, 4)) return -14;
  return data ? -22 : 0;
}
int64_t capability_get(uint64_t h, uint64_t data) {
  struct cap_header hdr;
  int n = words(h, data, &hdr);
  if (n <= 0 || !data) return n < 0 ? n : 0;
  if (hdr.pid < 0) return -22;
  struct thread *t = sched_get_current();
  if (!t) return -3;
  struct cap_data out[2] = {0};
  spinlock_acquire(&tid_lock);
  if (hdr.pid && hdr.pid != (int32_t)pid_namespace_tid(t, t)) {
    for (t = global_thread_list; t && pid_namespace_tid(sched_get_current(), t) != (uint32_t)hdr.pid; t = t->global_next) {}
  }
  if (!t) { spinlock_release(&tid_lock); return -3; }
  ensure(t);
  for (int i = 0; i < n; i++) {
    out[i].effective = t->cap_effective >> (32 * i);
    out[i].permitted = t->cap_permitted >> (32 * i);
    out[i].inheritable = t->cap_inheritable >> (32 * i);
  }
  spinlock_release(&tid_lock);
  return copy_to_user((void *)data, out, n * sizeof(out[0])) ? -14 : 0;
}
int64_t capability_set(uint64_t h, uint64_t data) {
  struct cap_header hdr;
  int n = words(h, data, &hdr);
  if (n <= 0) return n < 0 ? n : -22;
  struct thread *t = sched_get_current();
  if (!t) return -3;
  if (hdr.pid && hdr.pid != (int32_t)pid_namespace_tid(t, t)) return -1;
  struct cap_data in[2] = {0};
  if (copy_from_user(in, (void *)data, n * sizeof(in[0]))) return -14;
  uint64_t e = in[0].effective | ((uint64_t)in[1].effective << 32);
  uint64_t p = in[0].permitted | ((uint64_t)in[1].permitted << 32);
  uint64_t i = in[0].inheritable | ((uint64_t)in[1].inheritable << 32);
  ensure(t);
  if ((e | p | i) & ~CAP_VALID_MASK) return -22;
  if ((e & ~p) || (p & ~t->cap_permitted)) return -1;
  uint64_t allowed_i = t->cap_inheritable | t->cap_permitted;
  if (capability_has(t, CAP_SETPCAP)) allowed_i |= t->cap_bounding;
  if ((i & ~allowed_i) || (i & ~(t->cap_inheritable | t->cap_bounding))) return -1;
  t->cap_effective = e; t->cap_permitted = p; t->cap_inheritable = i;
  t->cap_ambient &= p & i;
  return 0;
}
