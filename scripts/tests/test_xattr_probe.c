/* Run the real diagnostic recorder and xattr store with mocked uaccess/logs.
 * Build with function/data sections and --gc-sections (see docs/xattr-probes.md). */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOCK_SPINLOCK_H
#undef strchr
#undef strrchr
#undef strstr
typedef struct { int locked; } spinlock_t;
#define SPINLOCK_INIT {0}
static void spinlock_acquire(spinlock_t *s) { assert(!s->locked); s->locked = 1; }
static void spinlock_release(spinlock_t *s) { assert(s->locked); s->locked = 0; }
#include "../../kernel/src/syscalls/sysdiag.c"
#include "../../kernel/src/fs/xattr.c"

static char output[8192];
static uint64_t now;
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
uint64_t lapic_timer_get_ms(void) { return ++now; }
const char *syscall_get_name(uint64_t n) { (void)n; return "xattr"; }
void klog_puts(const char *s) {
  assert(strlen(output) + strlen(s) < sizeof(output)); strcat(output, s);
}
void klog_uint64(uint64_t n) { (void)n; }
void klog_hex64(uint64_t n) { (void)n; }
unsigned long copy_from_user(void *to, const void *from, unsigned long n) {
  assert(!g_sysdiag_lock.locked);
  if ((uintptr_t)from < 4096) return n;
  memcpy(to, from, n); return 0;
}
long strncpy_from_user(char *dst, const char *src, long count) {
  assert(!g_sysdiag_lock.locked);
  if ((uintptr_t)src < 4096) return -14;
  for (long i = 0; i < count; i++) { dst[i] = src[i]; if (!src[i]) return i; }
  return count;
}
static void reset(void) {
  memset(g_syscall_errors, 0, sizeof(g_syscall_errors));
  g_syscall_error_count = g_syscall_error_total_calls = 0;
  output[0] = 0;
}
int main(void) {
  struct thread worker = {0}; strcpy(worker.comm, "KIO::WorkerThre");
  char path[128] = "/tmp/example";
  struct syscall_regs regs = {.rdi = (uintptr_t)path};
  const char *acl[] = {"system.posix_acl_access", "system.posix_acl_default"};
  for (unsigned n = 191; n <= 193; n++) {
    for (unsigned i = 0; i < 2; i++) {
      reset(); regs.rsi = (uintptr_t)acl[i]; regs.rdx = regs.r10 = 0;
      sysdiag_record_syscall_error(n, -95, &worker, &regs);
      assert(g_syscall_error_count == 1 && !g_syscall_errors[0].is_severe);
      assert(g_syscall_errors[0].total_calls == 1 && !output[0]);
      /* A different unsupported attribute sharing the same aggregate key
       * must still produce a diagnostic and promote its severity. */
      regs.rsi = (uintptr_t)"security.capability";
      sysdiag_record_syscall_error(n, -95, &worker, &regs);
      assert(g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 2);
      assert(strstr(output, "SYSCALL ERROR") && strstr(output, "xattr='security.capability'"));
      if (n != 193) assert(strstr(output, "path='/tmp/example'"));
    }
  }
  regs.rsi = (uintptr_t)acl[0]; regs.rdx = (uintptr_t)path; regs.r10 = 8;
  reset(); sysdiag_record_syscall_error(191, -95, &worker, &regs);
  assert(!g_syscall_errors[0].is_severe && !output[0]);
  regs.rdx = regs.r10 = 0;
  reset(); sysdiag_record_syscall_error(188, -95, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && output[0]);
  /* Unreadable and unterminated names never suppress an error. */
  reset(); regs.rsi = 1; sysdiag_record_syscall_error(191, -95, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && output[0]);
  char longname[256]; memset(longname, 'a', sizeof(longname));
  memcpy(longname, acl[0], strlen(acl[0]));
  reset(); regs.rsi = (uintptr_t)longname;
  sysdiag_record_syscall_error(191, -95, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && output[0]);
  reset(); regs.rsi = (uintptr_t)"system.posix_acl_access.extra";
  sysdiag_record_syscall_error(191, -95, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && output[0]);
  /* Access errors sharing one aggregation key must still promote real
   * permission denials after a harmless non-executable-file probe. */
  const unsigned access_calls[] = {21, 269, 439};
  for (unsigned i = 0; i < 3; i++) {
    unsigned n = access_calls[i];
    reset(); memset(&regs, 0, sizeof(regs));
    if (n == 21) { regs.rdi = (uintptr_t)path; regs.rsi = 1; }
    else { regs.rdi = (uint64_t)-100; regs.rsi = (uintptr_t)path; regs.rdx = 1; }
    worker.last_access_nonexec_probe = true;
    sysdiag_record_syscall_error(n, -13, &worker, &regs);
    assert(!worker.last_access_nonexec_probe && !output[0]);
    assert(g_syscall_error_count == 1 && !g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 1);
    /* Same mode, but no resolved non-executable-file evidence: e.g. directory. */
    sysdiag_record_syscall_error(n, -13, &worker, &regs);
    assert(g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 2 && output[0]);
    /* A later benign query must not downgrade the severe aggregate. */
    worker.last_access_nonexec_probe = true;
    sysdiag_record_syscall_error(n, -13, &worker, &regs);
    assert(g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 3);
    reset(); worker.last_access_nonexec_probe = true;
    if (n == 21) regs.rsi = 2; else regs.rdx = 2;
    sysdiag_record_syscall_error(n, -13, &worker, &regs);
    assert(g_syscall_errors[0].is_severe && output[0]);
  }
  reset(); worker.last_access_nonexec_probe = true;
  regs.rdi = (uintptr_t)path;
  sysdiag_record_syscall_error(2, -13, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && output[0]);
  puts("PASS: confirmed access execute probes remain counted quietly; directory/read/write/open denials promote or retain warnings");
  char theme_path[128] = "/usr/share/icons/breeze/index.theme";
  char config_path[128] = "/var/lib/sddm/.config/example";
  char theme_extra[128] = "/usr/share/icons/breeze/index.theme.extra";
  strcpy(worker.comm, "sddm-greeter-qt");
  memset(&regs, 0, sizeof(regs));
  regs.rdi = (uintptr_t)theme_path; regs.rsi = 2;
  reset(); sysdiag_record_syscall_error(21, -13, &worker, &regs);
  assert(!output[0] && !g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 1);
  regs.rdi = (uintptr_t)config_path;
  sysdiag_record_syscall_error(21, -13, &worker, &regs);
  assert(output[0] && g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 2);
  assert(strstr(output, "path='/var/lib/sddm/.config/example'"));
  regs.rdi = (uintptr_t)theme_path;
  const unsigned denied_modes[] = {1, 4, 6};
  for (unsigned i = 0; i < 3; i++) {
    reset(); regs.rsi = denied_modes[i];
    sysdiag_record_syscall_error(21, -13, &worker, &regs);
    assert(output[0] && g_syscall_errors[0].is_severe);
  }
  reset(); regs.rsi = 2; sysdiag_record_syscall_error(2, -13, &worker, &regs);
  assert(output[0] && g_syscall_errors[0].is_severe);
  reset(); regs.rdi = 1; sysdiag_record_syscall_error(21, -13, &worker, &regs);
  assert(output[0] && g_syscall_errors[0].is_severe);
  reset(); regs.rdi = (uintptr_t)theme_extra;
  sysdiag_record_syscall_error(21, -13, &worker, &regs);
  assert(output[0] && g_syscall_errors[0].is_severe);
  reset(); strcpy(worker.comm, "other");
  regs.rdi = (uintptr_t)theme_path;
  sysdiag_record_syscall_error(21, -13, &worker, &regs);
  assert(output[0] && g_syscall_errors[0].is_severe);
  puts("PASS: SDDM theme writability probe counted quietly; unrelated paths, modes, callers and actual opens retain warnings");
  strcpy(worker.comm, "firefox");
  memset(&regs, 0, sizeof(regs)); regs.rdi = CLONE_NEWUSER | 17;
  reset(); sysdiag_record_syscall_error(56, -1, &worker, &regs);
  assert(!output[0] && !g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 1);
  /* A real launch sharing the aggregate key must still promote severity. */
  regs.rsi = 4096; sysdiag_record_syscall_error(56, -1, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 2);
  assert(strstr(output, "UNSUPPORTED NAMESPACES") && strstr(output, "namespace isolation is not implemented"));
  reset(); regs.rsi = 0; sysdiag_record_syscall_error(56, -22, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && strstr(output, "SYSCALL ERROR"));
  reset(); strcpy(worker.comm, "bwrap"); regs.rdi = 0x6e020011;
  sysdiag_record_syscall_error(56, -1, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && strstr(output, "UNSUPPORTED NAMESPACES"));
  reset(); regs.rdi = CLONE_NEWUSER | 17;
  sysdiag_record_syscall_error(56, -1, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && output[0]);
  puts("PASS: exact Firefox user namespace probe counted quietly; real clone and bwrap failures remain visible");
  strcpy(worker.comm, "crashhelper");
  memset(&regs, 0, sizeof(regs)); regs.r10 = 0x0fff; regs.r9 = 0x0101010101010100;
  reset(); sysdiag_record_syscall_error(332, -14, &worker, &regs);
  assert(!output[0] && !g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 1);
  regs.rsi = (uintptr_t)path;
  sysdiag_record_syscall_error(332, -14, &worker, &regs);
  assert(output[0] && g_syscall_errors[0].is_severe && g_syscall_errors[0].total_calls == 2);
  /* Every significant argument must match the documented probe. */
  for (unsigned i = 0; i < 5; i++) {
    memset(&regs, 0, sizeof(regs)); regs.r10 = 0x0fff;
    switch (i) {
    case 0: regs.rdi = 3; break;
    case 1: regs.rsi = (uintptr_t)path; break;
    case 2: regs.rdx = 0x1000; break;
    case 3: regs.r10 = 0; break;
    case 4: regs.r8 = (uintptr_t)path; break;
    }
    reset(); sysdiag_record_syscall_error(332, -14, &worker, &regs);
    assert(output[0] && g_syscall_errors[0].is_severe);
  }
  reset(); memset(&regs, 0, sizeof(regs)); regs.r10 = 0x0fff;
  sysdiag_record_syscall_error(332, -1, &worker, &regs);
  assert(output[0] && g_syscall_errors[0].is_severe);
  reset(); strcpy(worker.comm, "firefox");
  regs.rdi = 17; regs.rsi = 270; regs.rdx = 12; regs.r10 = (uintptr_t)path; regs.r8 = 4;
  sysdiag_record_syscall_error(54, -92, &worker, &regs);
  assert(g_syscall_errors[0].is_severe && strstr(output, "NETLINK_GET_STRICT_CHK"));
  assert(strstr(output, "strict Netlink request validation is not implemented"));
  puts("PASS: exact Rust statx availability probe quiet; real statx faults and unsupported strict Netlink option remain visible");
  vfs_node_t node = {0};
  for (unsigned i = 0; i < 2; i++) {
    assert(vfs_getxattr(&node, acl[i], NULL, 0) == -95);
    assert(vfs_setxattr(&node, acl[i], "x", 1, 0) == -95);
  }
  assert(vfs_getxattr(&node, "user.test", NULL, 0) == -61);
  assert(vfs_setxattr(&node, "user.test", "ok", 2, 0) == 0);
  assert(vfs_getxattr(&node, "user.test", NULL, 0) == 2);
  char value[2]; assert(vfs_getxattr(&node, "user.test", value, sizeof(value)) == 2);
  assert(!memcmp(value, "ok", 2)); vfs_xattr_clear(&node);
  puts("PASS: ACL size and buffered-read probes remain tracked without warnings; other xattr failures stay visible");
  puts("PASS: fault/truncation handling and unsupported ACL/user attribute semantics");
  return 0;
}
