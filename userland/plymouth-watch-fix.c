#define _GNU_SOURCE
/* LD_PRELOAD shim to keep plymouthd's graphical splash alive on AvoryOS.
 *
 * Root cause (from serial fault dump):
 *   ply_event_loop_stop_watching_fd(loop, watch) crashes with
 *   watch=0x... in libxkbcommon data -> dst=libply ELF+0x20 -> src=0x40
 *   (e_phoff) -> mov ecx,[rbp] faults at 0x40.
 *   This is a use-after-free / fd-reuse re-entrancy: terminal-session
 *   hangup detaches (closes fd 9, pair1) and synchronously reattaches
 *   (opens fd 9, pair2). The new watch for fd 9 reuses the dying source
 *   for fd 9, which is then freed, leaving session->fd_watch dangling.
 *   Later teardown dereferences the dangling watch and kills the splash.
 *
 * Fix: guard the teardown with a SIGSEGV catch. Stale watches are ignored
 * (session already NULLs fd_watch after), letting boot continue with DRM
 * splash (1280x800, DIRTYFB) instead of terminating plymouthd.
 */
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <unistd.h>

typedef void (*stop_fn_t)(void *loop, void *watch);
static stop_fn_t real_stop = NULL;

static __thread sigjmp_buf guard_env;
static __thread volatile int guard_active = 0;

static void guard_handler(int sig, siginfo_t *info, void *ctx) {
  (void)sig; (void)info; (void)ctx;
  if (guard_active)
    siglongjmp(guard_env, 1);
}

void ply_event_loop_stop_watching_fd(void *loop, void *watch) {
  if (real_stop == NULL) {
    real_stop = (stop_fn_t)dlsym(RTLD_NEXT, "ply_event_loop_stop_watching_fd");
    if (real_stop == NULL)
      return;
  }
  /* NULL is a programming error; let libply assert handle it. Only guard
   * non-NULL wild pointers that would SIGSEGV. */
  if (loop == NULL || watch == NULL) {
    real_stop(loop, watch);
    return;
  }

  struct sigaction old_act, new_act;
  memset(&new_act, 0, sizeof(new_act));
  new_act.sa_sigaction = guard_handler;
  new_act.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&new_act.sa_mask);
  if (sigaction(SIGSEGV, &new_act, &old_act) != 0) {
    real_stop(loop, watch);
    return;
  }

  guard_active = 1;
  if (sigsetjmp(guard_env, 1) == 0) {
    real_stop(loop, watch);
    guard_active = 0;
    sigaction(SIGSEGV, &old_act, NULL);
  } else {
    /* Wild watch (e.g. 0x40 chain). Ignore teardown; caller NULLs its
     * fd_watch and the splash keeps running. */
    guard_active = 0;
    sigaction(SIGSEGV, &old_act, NULL);
    const char msg[] = "[plymouth-fix] ignored stale fd_watch teardown\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
  }
}
