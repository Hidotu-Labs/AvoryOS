#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t got_sig = 0;

static void handler(int sig, siginfo_t *si, void *uc) {
  (void)uc;
  if (sig == SIGUSR1 && si && si->si_signo == SIGUSR1)
    got_sig++;
}

int main(void) {
  struct sigaction sa = {0};
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(SIGUSR1, &sa, NULL) != 0) {
    perror("sigaction");
    return 1;
  }

  /* Verify register preservation across signal delivery + rt_sigreturn. */
  register uint64_t r12 asm("r12") = 0x1122334455667788ULL;
  register uint64_t r13 asm("r13") = 0x99AABBCCDDEEFF00ULL;
  __asm__ volatile("" : "+r"(r12), "+r"(r13));

  if (raise(SIGUSR1) != 0) {
    perror("raise");
    return 1;
  }

  __asm__ volatile("" : "+r"(r12), "+r"(r13));
  if (got_sig != 1 || r12 != 0x1122334455667788ULL ||
      r13 != 0x99AABBCCDDEEFF00ULL) {
    fprintf(stderr, "FAIL: sig=%d r12=0x%llx r13=0x%llx\n", got_sig,
            (unsigned long long)r12, (unsigned long long)r13);
    return 1;
  }

  /* Also verify fork() + child raise(SIGUSR1) + rt_sigreturn. */
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    if (raise(SIGUSR1) != 0)
      _exit(2);
    _exit(got_sig == 2 ? 0 : 3);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    fprintf(stderr, "FAIL: child status=0x%x\n", status);
    return 1;
  }

  printf("[FRED USER TEST] PASS (sigreturn + fork)\n");
  return 0;
}
