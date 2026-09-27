/* mocktail-stack-shim.c - LD_PRELOAD workaround for mocktail's undersized
 * pthread stacks on AvoryOS.
 *
 * Mocktail creates its threads (including the 'Main' thread) with ~5 MB
 * guardless stacks (pthread attrs: small stacksize, guardsize 0). Its startup
 * call chain legitimately needs ~5 MB + a few KB, so the leaf DNS frame in
 * __libc_ns_samename pushes RSP 560 B past the bottom and the thread dies
 * with SIGSEGV. The kernel cannot legitimately enlarge an app-sized stack,
 * so this shim raises the floor instead: any thread created with less than
 * 8 MB of stack or less than one page of guard gets bumped to 8 MB + 4 KB.
 * Larger stacks (e.g. the 64 MB / 1 GB workers) and explicit stack mappings
 * are left untouched.
 *
 * Disable at runtime with MOCKTAIL_STACK_SHIM_OFF=1.
 *
 * Build (glibc toolchain, NOT musl):
 *   x86_64-buildroot-linux-gnu-gcc -shared -fPIC -O2 \
 *     --sysroot=toolchain/glibc-sysroot \
 *     scripts/mocktail-stack-shim.c -o mocktail-stack-shim.so -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>

#define SHIM_MIN_STACK (8u * 1024u * 1024u)
#define SHIM_MIN_GUARD 4096u

static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *,
                                  void *(*)(void *), void *) = NULL;

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
  if (!real_pthread_create)
    real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");

  if (getenv("MOCKTAIL_STACK_SHIM_OFF"))
    return real_pthread_create(thread, attr, start, arg);

  pthread_attr_t tmp;
  const pthread_attr_t *use = attr;

  if (attr) {
    /* An explicitly mapped stack (setstack) cannot be resized or guarded
     * after the fact; leave those threads exactly alone. */
    void *stackaddr = NULL;
    size_t stacksize = 0;
    if (pthread_attr_getstack(attr, &stackaddr, &stacksize) == 0 &&
        stackaddr != NULL)
      return real_pthread_create(thread, attr, start, arg);

    size_t cur_stack = 0, cur_guard = 0;
    pthread_attr_getstacksize(attr, &cur_stack);
    pthread_attr_getguardsize(attr, &cur_guard);
    if (cur_stack >= SHIM_MIN_STACK && cur_guard >= SHIM_MIN_GUARD)
      return real_pthread_create(thread, attr, start, arg);

    tmp = *attr; /* pthread_attr_t is a plain value struct; shallow copy. */
    if (cur_stack < SHIM_MIN_STACK)
      pthread_attr_setstacksize(&tmp, SHIM_MIN_STACK);
    if (cur_guard < SHIM_MIN_GUARD)
      pthread_attr_setguardsize(&tmp, SHIM_MIN_GUARD);
    use = &tmp;
  } else {
    pthread_attr_init(&tmp);
    pthread_attr_setstacksize(&tmp, SHIM_MIN_STACK);
    pthread_attr_setguardsize(&tmp, SHIM_MIN_GUARD);
    use = &tmp;
  }

  return real_pthread_create(thread, use, start, arg);
}
