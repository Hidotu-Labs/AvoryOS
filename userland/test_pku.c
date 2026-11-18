#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_pkey_mprotect
#define SYS_pkey_mprotect 329
#endif
#ifndef SYS_pkey_alloc
#define SYS_pkey_alloc 330
#endif
#ifndef SYS_pkey_free
#define SYS_pkey_free 331
#endif

static inline uint32_t read_pkru(void) {
  uint32_t eax, edx;
  __asm__ volatile(".byte 0x0f, 0x01, 0xee" : "=a"(eax), "=d"(edx) : "c"(0));
  return eax;
}

static inline void write_pkru(uint32_t val) {
  __asm__ volatile(".byte 0x0f, 0x01, 0xef"
                   :
                   : "a"(val), "c"(0), "d"(0)
                   : "memory");
}

int main(void) {
  long pkey = syscall(SYS_pkey_alloc, 0, 0);
  if (pkey < 0) {
    if (errno == ENOSYS) {
      printf("[PKU TEST] SKIP: PKU not supported on this CPU\n");
      return 0;
    }
    perror("pkey_alloc");
    return 1;
  }

  volatile uint64_t *page =
      mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  *page = 0xCAFEBABE12345678ULL;

  if (syscall(SYS_pkey_mprotect, page, 4096, PROT_READ | PROT_WRITE, pkey) != 0) {
    perror("pkey_mprotect");
    return 1;
  }

  uint32_t base_pkru = read_pkru();

  /* 1. Disable writes for pkey: read must succeed, write must SIGSEGV. */
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    write_pkru(base_pkru | (2U << (pkey * 2)));
    if (*page != 0xCAFEBABE12345678ULL)
      _exit(2);
    *page = 0xDEADBEEF00000001ULL;
    _exit(3);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) ||
      WTERMSIG(status) != SIGSEGV) {
    fprintf(stderr, "FAIL: write with PKEY_DISABLE_WRITE status=0x%x\n", status);
    return 1;
  }

  /* 2. Disable all access for pkey: read must SIGSEGV. */
  pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    write_pkru(base_pkru | (1U << (pkey * 2)));
    uint64_t v = *page;
    (void)v;
    _exit(3);
  }
  status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) ||
      WTERMSIG(status) != SIGSEGV) {
    fprintf(stderr, "FAIL: read with PKEY_DISABLE_ACCESS status=0x%x\n", status);
    return 1;
  }

  /* 3. Verify normal access in parent + free. */
  write_pkru(base_pkru);
  *page = 0x123456789ABCDEF0ULL;
  if (*page != 0x123456789ABCDEF0ULL) {
    fprintf(stderr, "FAIL: readback in parent\n");
    return 1;
  }

  if (syscall(SYS_pkey_free, pkey) != 0) {
    perror("pkey_free");
    return 1;
  }
  munmap((void *)page, 4096);

  printf("[PKU TEST] PASS (pkey=%ld)\n", pkey);
  return 0;
}
