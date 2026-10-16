#include "seccomp.h"
#include "capability.h"
#include "pid_namespace.h"
#include "sched_internal.h"
#include "../syscalls/syscall.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../console/klog.h"
#include "../hal/hal.h"
#include "arch/uaccess.h"
void signal_send(struct thread *t, int sig);

#define SC_ALLOW 0x7fff0000U
#define SC_ERRNO 0x00050000U
#define SC_TRAP 0x00030000U
#define SC_KILL_PROCESS 0x80000000U
#define SC_MASK 0xffff0000U
#define SC_MAX_INSNS 4096
#define SC_MAX_PATH 32768

struct sc_instruction { uint16_t code; uint8_t jt, jf; uint32_t k; };
struct sc_data { uint32_t nr, arch; uint64_t ip, args[6]; };
struct seccomp_filter {
  uint32_t refs, count, path_cost;
  struct seccomp_filter *prev;
  struct sc_instruction insn[];
};
_Static_assert(sizeof(struct sc_data) == 64, "seccomp_data ABI");
_Static_assert(sizeof(struct sc_instruction) == 8, "sock_filter ABI");

static void sc_get(struct seccomp_filter *f) {
  if (f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
}
static void sc_put(struct seccomp_filter *f) {
  while (f && __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    struct seccomp_filter *prev = f->prev;
    kfree(f);
    f = prev;
  }
}

/* The registry lock also serializes filter publication, inheritance and TSYNC.
 * A running task pins its immutable chain before dropping that lock. */
void seccomp_inherit(struct thread *child, struct thread *parent) {
  spinlock_acquire(&tid_lock);
  struct seccomp_filter *old = child->seccomp_filter;
  child->no_new_privs = child->no_new_privs || parent->no_new_privs;
  child->seccomp_filter = parent->seccomp_filter;
  sc_get(child->seccomp_filter);
  spinlock_release(&tid_lock);
  sc_put(old);
}

void seccomp_release_task(struct thread *t) {
  spinlock_acquire(&tid_lock);
  struct seccomp_filter *old = t->seccomp_filter;
  t->seccomp_filter = NULL;
  spinlock_release(&tid_lock);
  sc_put(old);
}

int seccomp_mode(struct thread *t) {
  return t && __atomic_load_n(&t->seccomp_filter, __ATOMIC_ACQUIRE) ? 2 : 0;
}

/* All branches are forward. Propagate the intersection of initialized scratch
 * words at joins, so no path may read uninitialized BPF memory. */
static int sc_verify(struct seccomp_filter *f) {
  uint32_t *states = kmalloc(f->count * sizeof(*states));
  if (!states) return -12;
  for (uint32_t i = 0; i < f->count; i++) states[i] = 0x10000U;
  states[0] = 0;
  int result = -22;
  for (uint32_t i = 0; i < f->count; i++) {
    struct sc_instruction p = f->insn[i];
    uint16_t code = p.code;
    uint32_t next[2] = {i + 1, i + 1}, branches = 1;
    uint32_t mask = states[i] & 0xffffU;
    bool reached = states[i] != 0x10000U;
    switch (code) {
    case 0x00: case 0x01: /* LD/LDX immediate */
    case 0x80: case 0x81: /* LD/LDX length */
    case 0x07: case 0x87: /* TAX/TXA */
      break;
    case 0x20: /* LD W ABS, native-endian seccomp_data words */
      if ((p.k & 3) || p.k > sizeof(struct sc_data) - 4) goto out;
      break;
    case 0x60: case 0x61: /* LD/LDX scratch */
      if (p.k >= 16 || (reached && !(mask & (1U << p.k)))) goto out;
      break;
    case 0x02: case 0x03: /* ST/STX */
      if (p.k >= 16) goto out;
      mask |= 1U << p.k;
      break;
    case 0x06: case 0x16: /* RET K/A */
      branches = 0;
      break;
    case 0x05: /* JA */
      if (p.k >= f->count - i - 1) goto out;
      next[0] = i + 1 + p.k;
      break;
    case 0x15: case 0x1d: case 0x25: case 0x2d:
    case 0x35: case 0x3d: case 0x45: case 0x4d:
      next[0] = i + 1 + p.jt;
      next[1] = i + 1 + p.jf;
      branches = 2;
      break;
    case 0x04: case 0x0c: case 0x14: case 0x1c:
    case 0x24: case 0x2c: case 0x44: case 0x4c:
    case 0x54: case 0x5c: case 0xa4: case 0xac:
    case 0x84: /* NEG */
    case 0x3c: case 0x9c: /* DIV/MOD X, guarded at runtime */
    case 0x6c: case 0x7c: /* shifts by X */
      break;
    case 0x34: case 0x94:
      if (!p.k) goto out;
      break;
    case 0x64: case 0x74:
      if (p.k >= 32) goto out;
      break;
    default: goto out;
    }
    for (uint32_t b = 0; b < branches; b++) {
      if (next[b] >= f->count) goto out;
      if (!reached) continue;
      if (states[next[b]] == 0x10000U) states[next[b]] = mask;
      else states[next[b]] &= mask;
    }
  }
  if ((f->insn[f->count - 1].code & 7) != 6) goto out;
  result = 0;
 out:
  kfree(states);
  return result;
}

static uint32_t sc_run(const struct seccomp_filter *f, const struct sc_data *d) {
  uint32_t a = 0, x = 0, mem[16] = {0};
  for (uint32_t pc = 0; pc < f->count; pc++) {
    struct sc_instruction p = f->insn[pc];
    uint32_t v = (p.code & 8) ? x : p.k;
    switch (p.code) {
    case 0x00: a = p.k; break;
    case 0x01: x = p.k; break;
    case 0x80: a = sizeof(*d); break;
    case 0x81: x = sizeof(*d); break;
    case 0x20: memcpy(&a, (const uint8_t *)d + p.k, 4); break;
    case 0x60: a = mem[p.k]; break;
    case 0x61: x = mem[p.k]; break;
    case 0x02: mem[p.k] = a; break;
    case 0x03: mem[p.k] = x; break;
    case 0x07: x = a; break;
    case 0x87: a = x; break;
    case 0x04: case 0x0c: a += v; break;
    case 0x14: case 0x1c: a -= v; break;
    case 0x24: case 0x2c: a *= v; break;
    case 0x34: case 0x3c: if (!v) return 0; a /= v; break;
    case 0x94: case 0x9c: if (!v) return 0; a %= v; break;
    case 0x44: case 0x4c: a |= v; break;
    case 0x54: case 0x5c: a &= v; break;
    case 0xa4: case 0xac: a ^= v; break;
    case 0x64: case 0x6c: a <<= (v & 31); break;
    case 0x74: case 0x7c: a >>= (v & 31); break;
    case 0x84: a = 0U - a; break;
    case 0x05: pc += p.k; break;
    case 0x15: case 0x1d: pc += a == v ? p.jt : p.jf; break;
    case 0x25: case 0x2d: pc += a > v ? p.jt : p.jf; break;
    case 0x35: case 0x3d: pc += a >= v ? p.jt : p.jf; break;
    case 0x45: case 0x4d: pc += (a & v) ? p.jt : p.jf; break;
    case 0x06: return p.k;
    case 0x16: return a;
    default: return 0;
    }
  }
  return 0;
}

static bool sc_action_supported(uint32_t action) {
  return action == 0 || action == SC_KILL_PROCESS || action == SC_TRAP ||
         action == SC_ERRNO || action == SC_ALLOW;
}

int64_t seccomp_control(uint32_t op, uint32_t flags, uint64_t args) {
  struct thread *current = sched_get_current();
  if (!current) return -3;
  if (op == 2) {
    if (flags) return -22;
    uint32_t action;
    if (copy_from_user(&action, (void *)args, 4)) return -14;
    return sc_action_supported(action) ? 0 : -95;
  }
  if (op != 1 || (flags & ~1U)) return -22;
  uint16_t count;
  uint64_t program = 0;
  if (copy_from_user(&count, (void *)args, 2)) return -14;
  if (current->mm && current->mm->compat32) {
    uint32_t ptr;
    if (copy_from_user(&ptr, (void *)(args + 4), 4)) return -14;
    program = ptr;
  } else if (copy_from_user(&program, (void *)(args + 8), 8)) return -14;
  if (!count || count > SC_MAX_INSNS) return -22;
  if (!current->no_new_privs && !capability_has(current, CAP_SYS_ADMIN)) return -13;
  struct seccomp_filter *f = kmalloc(sizeof(*f) + count * sizeof(f->insn[0]));
  if (!f) return -12;
  memset(f, 0, sizeof(*f));
  f->refs = 1;
  f->count = count;
  if (copy_from_user(f->insn, (void *)program, count * sizeof(f->insn[0]))) {
    sc_put(f); return -14;
  }
  int error = sc_verify(f);
  if (error) { sc_put(f); return error; }
  spinlock_acquire(&tid_lock);
  struct seccomp_filter *old = current->seccomp_filter;
  f->path_cost = count + (old ? old->path_cost + 4 : 0);
  if (f->path_cost > SC_MAX_PATH) {
    spinlock_release(&tid_lock); sc_put(f); return -12;
  }
  if (flags & 1) {
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
      if (t->tgid != current->tgid || t->state == THREAD_DEAD ||
          t->state == THREAD_ZOMBIE) continue;
      /* Linux permits synchronization from an ancestor chain as well. */
      struct seccomp_filter *ancestor = old;
      while (ancestor && ancestor != t->seccomp_filter) ancestor = ancestor->prev;
      if (t->seccomp_filter && !ancestor) {
        int64_t tid = pid_namespace_tid(current, t);
        spinlock_release(&tid_lock); sc_put(f); return tid;
      }
    }
  }
  f->prev = old;
  sc_get(old);
  if (flags & 1) {
    for (struct thread *t = global_thread_list; t; t = t->global_next) {
      if (t->tgid != current->tgid || t->state == THREAD_DEAD ||
          t->state == THREAD_ZOMBIE) continue;
      struct seccomp_filter *prior = t->seccomp_filter;
      sc_get(f);
      __atomic_store_n(&t->seccomp_filter, f, __ATOMIC_RELEASE);
      if (current->no_new_privs) t->no_new_privs = true;
      sc_put(prior); /* Held alive by f->prev while under the registry lock. */
    }
  } else {
    sc_get(f);
    __atomic_store_n(&current->seccomp_filter, f, __ATOMIC_RELEASE);
    sc_put(old);
  }
  spinlock_release(&tid_lock);
  sc_put(f);
  return 0;
}

static void sc_kill(struct thread *t, bool group) {
  t->termination_signal = 31;
  t->exit_status = 159;
  if (group) sched_terminate_thread_group(t);
  process_do_exit(159);
}

bool seccomp_syscall(struct thread *t, struct syscall_regs *regs,
                      uint32_t nr, uint32_t arch, const uint64_t args[6]) {
  if (!t || !__atomic_load_n(&t->seccomp_filter, __ATOMIC_ACQUIRE)) return true;
  spinlock_acquire(&tid_lock);
  struct seccomp_filter *head = t->seccomp_filter;
  sc_get(head);
  spinlock_release(&tid_lock);
  struct sc_data data = {.nr = nr, .arch = arch, .ip = regs->rip};
  memcpy(data.args, args, sizeof(data.args));
  uint32_t result = SC_ALLOW;
  for (struct seccomp_filter *f = head; f; f = f->prev) {
    uint32_t r = sc_run(f, &data);
    uint32_t action = r & SC_MASK;
    if (!sc_action_supported(action)) r = SC_KILL_PROCESS;
    if ((int32_t)(r & SC_MASK) < (int32_t)(result & SC_MASK)) result = r;
  }
  sc_put(head);
  switch (result & SC_MASK) {
  case SC_ALLOW: return true;
  case SC_ERRNO: {
    uint32_t error = result & 0xffffU;
    if (error > 4095) error = 4095;
    regs->rax = (uint64_t)-(int64_t)error;
    return false;
  }
  case SC_TRAP:
    t->seccomp_sigsys = true;
    t->seccomp_trap_data = result & 0xffffU;
    t->seccomp_trap_arch = arch;
    t->syscall_dispatch_call_addr = regs->rip;
    t->syscall_dispatch_number = (int32_t)nr;
    /* A synchronous SIGSYS cannot be ignored or kept blocked. */
    if (t->signal_handlers[30].sa_handler == (void *)SIG_IGN)
      t->signal_handlers[30].sa_handler = (void *)SIG_DFL;
    t->signal_mask &= ~(1ULL << 30);
    signal_send(t, 31);
    return false;
  case 0: sc_kill(t, false); return false;
  default: sc_kill(t, true); return false;
  }
}
