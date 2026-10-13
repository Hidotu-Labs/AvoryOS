#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

/* Put the normal pthread stack below its alternate stack in one mapping.
 * This deterministically reproduces the Firefox/glycin signal-delivery bug. */
#define STACK_SIZE (128 * 1024)
static unsigned char *stacks;
static volatile sig_atomic_t normal_seen, alt_seen, nested_seen, bad_stack;
static volatile sig_atomic_t trap_seen;

static void trap_handler(int sig, siginfo_t *info, void *context) {
    volatile unsigned char marker;
    ucontext_t *uc = context;
    if (sig != SIGILL || info->si_addr != (void *)uc->uc_mcontext.gregs[REG_RIP] ||
        (uintptr_t)&marker < (uintptr_t)(stacks + STACK_SIZE) ||
        (uintptr_t)&marker >= (uintptr_t)(stacks + 2 * STACK_SIZE))
        bad_stack = 1;
    /* Firefox's Wasm handler similarly resumes by editing the saved RIP. */
    uc->uc_mcontext.gregs[REG_RIP] += 2;
    trap_seen++;
}

static int in_altstack(uintptr_t sp) {
    return sp >= (uintptr_t)(stacks + STACK_SIZE) &&
           sp < (uintptr_t)(stacks + 2 * STACK_SIZE);
}

static void normal_handler(int sig) {
    volatile unsigned char marker;
    (void)sig;
    uintptr_t sp = (uintptr_t)&marker;
    if (sp < (uintptr_t)stacks || sp >= (uintptr_t)(stacks + STACK_SIZE))
        bad_stack = 1;
    normal_seen++;
}

static void nested_handler(int sig) {
    volatile unsigned char marker;
    (void)sig;
    if (!in_altstack((uintptr_t)&marker)) bad_stack = 1;
    nested_seen++;
}

static void alt_handler(int sig) {
    volatile unsigned char marker;
    (void)sig;
    if (!in_altstack((uintptr_t)&marker)) bad_stack = 1;
    alt_seen++;
    /* SIGUSR2 has no SA_ONSTACK, but must remain on this stack when nested. */
    if (raise(SIGUSR2) != 0) bad_stack = 1;
}

static void *worker(void *unused) {
    (void)unused;
    stack_t ss = {.ss_sp = stacks + STACK_SIZE, .ss_size = STACK_SIZE};
    struct sigaction sa = {0};
    sigemptyset(&sa.sa_mask);
    if (sigaltstack(&ss, NULL) != 0) return (void *)1;

    sa.sa_handler = normal_handler;
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || raise(SIGUSR1) != 0)
        return (void *)1;
    if (normal_seen != 1 || bad_stack) return (void *)1;

    sa.sa_handler = nested_handler;
    if (sigaction(SIGUSR2, &sa, NULL) != 0) return (void *)1;
    sa.sa_handler = alt_handler;
    sa.sa_flags = SA_ONSTACK;
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || raise(SIGUSR1) != 0)
        return (void *)1;
    if (alt_seen != 1 || nested_seen != 1 || bad_stack) return (void *)1;

    sa.sa_sigaction = trap_handler;
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    if (sigaction(SIGILL, &sa, NULL) != 0) return (void *)1;
    __asm__ volatile("ud2" ::: "memory");
    __asm__ volatile("ud2" ::: "memory");
    if (trap_seen != 2 || bad_stack) return (void *)1;

    /* Verify signal return restored the normal stack and signal mask. */
    sa.sa_handler = normal_handler;
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || raise(SIGUSR1) != 0)
        return (void *)1;
    ss.ss_flags = SS_DISABLE;
    if (sigaltstack(&ss, NULL) != 0) return (void *)1;
    /* SA_ONSTACK falls back to the normal stack when no alternate is enabled. */
    sa.sa_flags = SA_ONSTACK;
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || raise(SIGUSR1) != 0)
        return (void *)1;
    return (void *)(uintptr_t)(normal_seen != 3 || bad_stack);
}

static void child_handler(int sig) { (void)sig; }

static int check_exec_stack(const char *path) {
    /* Keep init's children waitable even when they exit before waitpid. */
    struct sigaction sa = {.sa_handler = child_handler}, saved;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, &saved) != 0) return 0;
    stack_t ss = {.ss_sp = stacks + STACK_SIZE, .ss_size = STACK_SIZE}, old;
    if (sigaltstack(&ss, NULL) != 0) return 0;
    pid_t child = fork();
    if (child == 0) {
        if (sigaltstack(NULL, &old) != 0 || old.ss_sp != ss.ss_sp ||
            old.ss_size != ss.ss_size || (old.ss_flags & SS_DISABLE)) _exit(2);
        char *args[] = {(char *)path, "--after-exec", NULL};
        execv("/no-such-signal-stack-test", args);
        if (errno != ENOENT || sigaltstack(NULL, &old) != 0 ||
            old.ss_sp != ss.ss_sp || (old.ss_flags & SS_DISABLE)) _exit(3);
        execv(path, args);
        _exit(4);
    }
    int status = 0;
    pid_t waited;
    do { waited = child > 0 ? waitpid(child, &status, 0) : -1; }
    while (child > 0 && waited < 0 && errno == EINTR);
    int wait_error = errno;
    int ok = child > 0 && waited == child &&
             WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) {
        FILE *log = fopen("/dev/kmsg", "w");
        if (log) {
            fprintf(log, "SIGNAL_STACK: exec child=%d waited=%d errno=%d status=%#x path=%s\n",
                    (int)child, (int)waited, wait_error, status, path);
            fclose(log);
        }
    }
    /* The parent's registration must survive its child's exec. */
    ok = ok && sigaltstack(NULL, &old) == 0 && old.ss_sp == ss.ss_sp &&
         !(old.ss_flags & SS_DISABLE);
    ss.ss_flags = SS_DISABLE;
    int disabled = sigaltstack(&ss, NULL) == 0;
    return sigaction(SIGCHLD, &saved, NULL) == 0 && disabled && ok;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--after-exec") == 0) {
        stack_t ss;
        return sigaltstack(NULL, &ss) != 0 || !(ss.ss_flags & SS_DISABLE) ||
               ss.ss_sp != NULL || ss.ss_size != 0;
    }
    pthread_attr_t attr;
    pthread_t thread;
    void *result = (void *)1;
    /* Also print to the serial device when used as a headless guest init. */
    FILE *serial = fopen("/dev/kmsg", "w");
    stacks = mmap(NULL, 2 * STACK_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ok = stacks != MAP_FAILED && pthread_attr_init(&attr) == 0;
    if (ok) {
        ok = pthread_attr_setstack(&attr, stacks, STACK_SIZE) == 0 &&
             pthread_create(&thread, &attr, worker, NULL) == 0;
        pthread_attr_destroy(&attr);
        if (ok) ok = pthread_join(thread, &result) == 0 && result == NULL;
    }
    if (ok) ok = check_exec_stack(argv[0]);
    const char *message = ok ?
        "SIGNAL_STACK: PASS (normal, alternate, nested, SIGILL recovery, return, disabled, fork/exec)\n" :
        "SIGNAL_STACK: FAIL\n";
    fputs(message, stdout);
    if (serial) { fputs(message, serial); fclose(serial); }
    if (stacks != MAP_FAILED) munmap(stacks, 2 * STACK_SIZE);
    return ok ? 0 : 1;
}
