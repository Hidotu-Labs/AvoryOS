#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

/* A known native interval, including the signal restorer. Test interception
 * with a separate syscall instruction outside that interval. */
extern char native_start[], native_end[], foreign_after[];
extern long native_call(long, long, long, long, long, long, long);
extern long foreign_call(long, long, long, long);
extern void dispatch_restorer(void);
__asm__(
".text\n"
".global native_start,native_end,native_call,dispatch_restorer\n"
"native_start:\n"
"native_call:\n"
"mov %rdi,%rax\nmov %rsi,%rdi\nmov %rdx,%rsi\nmov %rcx,%rdx\n"
"mov %r8,%r10\nmov %r9,%r8\nmov 8(%rsp),%r9\nsyscall\nret\n"
"dispatch_restorer:\nmov $15,%rax\nsyscall\nud2\n"
"native_end:\n"
".global foreign_call,foreign_after\n"
"foreign_call:\nmov %rdi,%rax\nmov %rsi,%rdi\nmov %rdx,%rsi\n"
"mov %rcx,%rdx\nsyscall\nforeign_after:\nret\n");
static volatile unsigned char selector = 1;
static volatile sig_atomic_t seen, bad;
static long expected_number;
static long expected_arg;
static void dispatch_handler(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (sig != SIGSYS || info->si_code != 2 || info->si_errno != 0 ||
        info->si_arch != 0xc000003eU || info->si_syscall != expected_number ||
        info->si_call_addr != foreign_after ||
        uc->uc_mcontext.gregs[REG_RIP] != (greg_t)foreign_after ||
        uc->uc_mcontext.gregs[REG_RAX] != expected_number ||
        uc->uc_mcontext.gregs[REG_RDI] != expected_arg ||
        uc->uc_mcontext.gregs[REG_RCX] != (greg_t)foreign_after)
        bad = 1;
    uc->uc_mcontext.gregs[REG_RAX] = 0x123456;
    seen++;
}
struct kernel_action {
    void (*handler)(int, siginfo_t *, void *);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask;
};
static void child_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info; (void)context;
}
static long configure(long mode, unsigned long offset, unsigned long len,
                      const void *key) {
    return native_call(SYS_prctl, 59, mode, offset, len, (long)key, 0);
}
static int enable(const void *key) {
    return configure(1, (uintptr_t)native_start,
                     (uintptr_t)native_end - (uintptr_t)native_start, key) == 0;
}
static int wait_ok(pid_t child, int expected) {
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    int ok = waited == child && ((WIFEXITED(status) && WEXITSTATUS(status) == expected) ||
                              (expected >= 128 && WIFSIGNALED(status) &&
                               WTERMSIG(status) == expected - 128));
    if (!ok) {
        FILE *log = fopen("/dev/kmsg", "w");
        if (log) {
            fprintf(log, "USER_DISPATCH: child=%d waited=%d errno=%d status=%#x expected=%d\n",
                    (int)child, (int)waited, errno, status, expected);
            fclose(log);
        }
    }
    return ok;
}
static int run_tests(const char *path) {
    struct kernel_action sa = {dispatch_handler, SA_SIGINFO | 0x04000000,
                               dispatch_restorer, 0};
    if (native_call(SYS_rt_sigaction, SIGSYS, (long)&sa, 0, 8, 0, 0)) return 1;
    /* AvoryOS automatically reaps default-disposition children of init when
     * init isn't waiting yet. Keep children waitable for this headless test. */
    sa.handler = child_handler;
    if (native_call(SYS_rt_sigaction, SIGCHLD, (long)&sa, 0, 8, 0, 0)) return 1;
    if (configure(0, 1, 0, NULL) != -EINVAL ||
        configure(3, 0, 0, NULL) != -EINVAL ||
        configure(1, 1, 0, NULL) != -EINVAL ||
        configure(1, UINT64_MAX, 2, NULL) != -EINVAL ||
        configure(1, 0, 0, (void *)UINT64_MAX) != -EFAULT) return 2;
    long pid = getpid();
    selector = 0;
    if (!enable((const void *)&selector)) return 3;
    if (foreign_call(SYS_getpid, 0, 0, 0) != pid || seen) return 4;
    selector = 1;
    if (native_call(SYS_getpid, 0, 0, 0, 0, 0, 0) != pid) return 5;
    expected_number = SYS_getpid; expected_arg = 0x7654321;
    if (foreign_call(SYS_getpid, expected_arg, 0, 0) != 0x123456 ||
        seen != 1 || bad) return 6;
    /* Foreign numbers beyond the Linux table must still reach the handler. */
    expected_number = 0x12345678;
    if (foreign_call(expected_number, expected_arg, 0, 0) != 0x123456 ||
        seen != 2 || bad) return 7;
    /* Intercept exit itself, proving that the native handler never ran. */
    expected_number = SYS_exit; expected_arg = 99;
    if (foreign_call(SYS_exit, 99, 0, 0) != 0x123456 || seen != 3 || bad) return 8;
    if (configure(0, 0, 0, NULL)) return 9;
    if (foreign_call(SYS_getpid, 0, 0, 0) != pid) return 10;
    /* NULL selector intercepts every call outside the native interval. */
    if (!enable(NULL)) return 11;
    expected_number = SYS_getpid; expected_arg = 0;
    if (foreign_call(SYS_getpid, 0, 0, 0) != 0x123456 || seen != 4 || bad) return 12;
    if (configure(0, 0, 0, NULL)) return 13;
    selector = 0;
    if (!enable((const void *)&selector)) return 14;
    /* Exercise both the raw fork and libc's clone-based fork entry points. */
    pid_t child = (pid_t)native_call(SYS_fork, 0, 0, 0, 0, 0, 0);
    if (!child) {
        selector = 1;
        _exit(foreign_call(SYS_getpid, 0, 0, 0) == getpid() ? 0 : 1);
    }
    if (child < 0 || !wait_ok(child, 0)) return 15;
    child = fork();
    if (!child) {
        if (!enable((const void *)&selector)) _exit(2);
        char *args[] = {(char *)path, "--after-exec", NULL};
        execv(path, args);
        _exit(3);
    }
    if (child < 0 || !wait_ok(child, 0)) return 16;
    child = fork();
    if (!child) {
        if (!enable((const void *)&selector)) _exit(2);
        selector = 2;
        foreign_call(SYS_getpid, 0, 0, 0);
        _exit(3);
    }
    if (child < 0 || !wait_ok(child, 128 + SIGSYS)) return 17;
    /* Selector can become unreadable after registration: catch the copy fault
     * and terminate the process, without panicking in kernel mode. */
    child = fork();
    if (!child) {
        void *key = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (key == MAP_FAILED || !enable(key)) _exit(2);
        foreign_call(SYS_getpid, 0, 0, 0);
        _exit(3);
    }
    if (child < 0 || !wait_ok(child, 128 + SIGSEGV)) return 18;
    child = fork();
    if (!child) {
        if (!enable((const void *)&selector)) _exit(2);
        struct kernel_action ignored = {(void *)SIG_IGN, 0, NULL, 0};
        native_call(SYS_rt_sigaction, SIGSYS, (long)&ignored, 0, 8, 0, 0);
        selector = 1;
        foreign_call(SYS_getpid, 0, 0, 0);
        _exit(3);
    }
    if (child < 0 || !wait_ok(child, 128 + SIGSYS)) return 20;
    child = fork();
    if (!child) {
        if (!enable((const void *)&selector)) _exit(2);
        unsigned long mask = 1UL << (SIGSYS - 1);
        native_call(SYS_rt_sigprocmask, SIG_BLOCK, (long)&mask, 0, 8, 0, 0);
        selector = 1;
        foreign_call(SYS_getpid, 0, 0, 0);
        _exit(3);
    }
    if (child < 0 || !wait_ok(child, 128 + SIGSYS)) return 21;
    if (configure(0, 0, 0, NULL)) return 19;
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--after-exec"))
        return foreign_call(SYS_getpid, 0, 0, 0) == getpid() ? 0 : 1;
    int result = run_tests(argv[0]);
    /* All printing occurs after disabling interception. */
    selector = 0;
    configure(0, 0, 0, NULL);
    FILE *log = fopen("/dev/kmsg", "w");
    printf("USER_DISPATCH: %s stage=%d\n", result ? "FAIL" : "PASS", result);
    if (log) {
        fprintf(log, "USER_DISPATCH: %s stage=%d\n", result ? "FAIL" : "PASS", result);
        fclose(log);
    }
    return result != 0;
}
