#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MADV_COLD
#define MADV_COLD 20
#endif
#define PAGE 4096UL
static const char *stage = "init";
static void fail(int line) {
  printf("ZRAM_BOOT: FAIL stage=%s line=%d errno=%d\n", stage, line, errno);
  fflush(stdout);
  sync();
  reboot(RB_POWER_OFF);
  _exit(1);
}
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      fail(__LINE__);                                                          \
  } while (0)
static uint64_t metric(const char *key) {
  FILE *f = fopen("/proc/zram", "r");
  CHECK(f);
  char name[80];
  unsigned long long value;
  while (fscanf(f, "%79s %llu", name, &value) == 2)
    if (!strcmp(name, key)) {
      fclose(f);
      return value;
    }
  fclose(f);
  fail(__LINE__);
  return 0;
}
/* Exec can retire its old mm to the deferred reaper. Wait for that
 * bounded background cleanup, then require every slot reference to be gone. */
static void await_empty(void) {
  for (unsigned i = 0; i < 5000; i++) {
    if (!metric("stored_pages"))
      return;
    usleep(1000);
  }
  CHECK(!metric("stored_pages"));
}
static uint64_t word(size_t page, unsigned i, uint64_t seed) {
  uint64_t x = (page + 1) * 0x9e3779b97f4a7c15ULL ^
               (i % 8) * 0xd6e8feb86659fd93ULL ^ seed;
  x ^= x >> 31;
  x *= 0x94d049bb133111ebULL;
  return x ^ (x >> 29);
}
static void fill(void *memory, size_t pages, uint64_t seed) {
  uint64_t *p = memory;
  for (size_t j = 0; j < pages; j++)
    for (unsigned i = 0; i < 512; i++)
      p[j * 512 + i] = word(j, i, seed);
}
static void verify_page(const uint64_t *p, size_t j, uint64_t seed,
                        int modified) {
  for (unsigned i = 0; i < 512; i++)
    CHECK(p[j * 512 + i] ==
          (word(j, i, seed) ^
           ((modified && i == 0) ? 0x123456789abcdef0ULL : 0)));
}
static void verify(const void *memory, size_t pages, uint64_t seed) {
  const uint64_t *p = memory;
  for (size_t j = 0; j < pages; j++)
    verify_page(p, j, seed, 0);
}
static void *mapping(size_t pages) {
  void *p = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(p != MAP_FAILED);
  return p;
}
static void fault_handler(int sig) {
  (void)sig;
  _exit(77);
}
static void protected_access(void *p, int write) {
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    signal(SIGSEGV, fault_handler);
    if (write)
      *(volatile char *)p = 1;
    else
      (void)*(volatile char *)p;
    _exit(42);
  }
  int status;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 77);
}
static void lifecycle(void) {
  stage = "cold/pageout/fork";
  size_t pages = 1024;
  uint64_t seed = 0xfeed1234;
  uint64_t *p = mapping(pages);
  fill(p, pages, seed);
  CHECK(!madvise(p, pages * PAGE, MADV_COLD));
  verify(p, pages, seed);
  uint64_t before = metric("swapouts");
  CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
  CHECK(metric("swapouts") >= before + pages);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    verify(p, pages, seed);
    for (size_t j = 0; j < pages; j++)
      p[j * 512] ^= 0x99;
    _exit(0);
  }
  int status;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        !WEXITSTATUS(status));
  verify(p, pages, seed);
  CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
  child = fork();
  CHECK(child >= 0);
  if (!child)
    _exit(0);
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        !WEXITSTATUS(status));
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    execl("/bin/probe-init", "probe-init", "exec-child", NULL);
    _exit(99);
  }
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        !WEXITSTATUS(status));
  CHECK(!mprotect(p, pages * PAGE, PROT_READ));
  protected_access(p, 1);
  verify(p, pages, seed);
  CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
  CHECK(!mprotect(p, pages * PAGE, PROT_NONE));
  protected_access(p, 0);
  CHECK(!mprotect(p, pages * PAGE, PROT_READ | PROT_WRITE));
  verify(p, pages, seed);
  CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
  CHECK(!madvise(p, 128 * PAGE, MADV_DONTNEED));
  for (size_t j = 0; j < 128 * 512; j++)
    CHECK(!p[j]);
  for (size_t j = 128; j < pages; j++)
    verify_page(p, j, seed, 0);
  CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
  CHECK(!munmap((char *)p + 256 * PAGE, 128 * PAGE));
  CHECK(mmap((char *)p + 256 * PAGE, 128 * PAGE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
             0) == (char *)p + 256 * PAGE);
  for (size_t j = 256 * 512; j < 384 * 512; j++)
    CHECK(!p[j]);
  for (size_t j = 384; j < pages; j++)
    verify_page(p, j, seed, 0);
  CHECK(!munmap(p, pages * PAGE));
  await_empty();
  puts("ZRAM_BOOT: PASS lifecycle: COLD, PAGEOUT, fork isolation, exit "
       "cleanup, permissions, DONTNEED, partial unmap and MAP_FIXED");

  stage = "mremap";
  pages = 256;
  void *arena = mmap(NULL, pages * PAGE * 3, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(arena != MAP_FAILED);
  p = mmap(arena, pages * PAGE, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK(p == arena);
  fill(p, pages, seed);
  CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
  void *moved = mremap(p, pages * PAGE, pages * PAGE * 2, MREMAP_MAYMOVE);
  CHECK(moved != MAP_FAILED && moved != p);
  verify(moved, pages, seed);
  uint64_t *extra = (uint64_t *)moved + pages * 512;
  for (size_t j = 0; j < pages * 512; j++)
    CHECK(!extra[j]);
  CHECK(!munmap(moved, pages * PAGE * 2));
  CHECK(!munmap((char *)arena + pages * PAGE, pages * PAGE * 2));
  await_empty();
  puts("ZRAM_BOOT: PASS compressed source survives forced mremap relocation");

  stage = "shared/incompressible/locking";
  pages = 256;
  p = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(p != MAP_FAILED);
  fill(p, pages, seed);
  before = metric("swapouts");
  CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
  CHECK(metric("swapouts") == before);
  verify(p, pages, seed);
  CHECK(!munmap(p, pages * PAGE));
  p = mapping(pages);
  uint64_t rng = 99;
  for (size_t i = 0; i < pages * 512; i++) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    p[i] = rng;
  }
  CHECK(!mprotect(p, pages * PAGE, PROT_READ));
  before = metric("swapouts");
  uint64_t rejects = metric("rejected_pages");
  CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
  CHECK(metric("swapouts") == before &&
        metric("rejected_pages") >= rejects + pages);
  protected_access(p, 1); /* rejected compression must not grant COW writes */
  rng = 99;
  for (size_t i = 0; i < pages * 512; i++) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    CHECK(p[i] == rng);
  }
  before = metric("swapouts");
  CHECK(!mlock(p, PAGE));
  CHECK(!madvise(p, PAGE, MADV_PAGEOUT));
  CHECK(metric("swapouts") == before);
  CHECK(!munlock(p, PAGE));
  CHECK(!munmap(p, pages * PAGE));
  puts("ZRAM_BOOT: PASS shared pages excluded, incompressible bytes retained, "
       "mlocked pages excluded from reclaim");
}
static void *concurrent(void *arg) {
  uintptr_t id = (uintptr_t)arg;
  size_t pages = 128;
  uint64_t *p = mapping(pages);
  for (unsigned turn = 0; turn < 16; turn++) {
    uint64_t seed = id * 100000 + turn;
    fill(p, pages, seed);
    CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
    CHECK(!mprotect(p, pages * PAGE, PROT_READ));
    verify(p, pages, seed);
    CHECK(!mprotect(p, pages * PAGE, PROT_READ | PROT_WRITE));
  }
  CHECK(!munmap(p, pages * PAGE));
  return NULL;
}
static atomic_int race_stop;
static void *race_address;
static void *pageout_racer(void *unused) {
  (void)unused;
  while (!atomic_load(&race_stop)) {
    CHECK(!madvise(race_address, 16 * PAGE, MADV_PAGEOUT));
    sched_yield();
  }
  return NULL;
}
static void parallel(void) {
  stage = "8-thread compression/fault races";
  pthread_t threads[8];
  for (uintptr_t i = 0; i < 8; i++)
    CHECK(!pthread_create(&threads[i], NULL, concurrent, (void *)(i + 1)));
  for (unsigned i = 0; i < 8; i++)
    CHECK(!pthread_join(threads[i], NULL));
  await_empty();
  stage = "PAGEOUT vs MAP_FIXED";
  race_address = mapping(16);
  pthread_t racer;
  CHECK(!pthread_create(&racer, NULL, pageout_racer, NULL));
  for (unsigned i = 0; i < 400; i++) {
    CHECK(mmap(race_address, 16 * PAGE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == race_address);
    fill(race_address, 16, i + 100);
    sched_yield();
    verify(race_address, 16, i + 100);
  }
  atomic_store(&race_stop, 1);
  CHECK(!pthread_join(racer, NULL));
  CHECK(!munmap(race_address, 16 * PAGE));
  await_empty();
  puts("ZRAM_BOOT: PASS 8-thread pageout/fault/protection stress and 400 "
       "concurrent fixed-remap replacements");
}
static size_t gcd(size_t a, size_t b) {
  while (b) {
    size_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}
struct fill_range {
  uint64_t *p;
  size_t first, end;
};
static void *pressure_fill(void *arg) {
  struct fill_range *r = arg;
  for (size_t j = r->first; j < r->end; j++)
    for (unsigned i = 0; i < 512; i++)
      r->p[j * 512 + i] = word(j, i, 0x12345678);
  return NULL;
}
static void pressure(void) {
  stage = "automatic oversubscription";
  struct sysinfo info;
  CHECK(!sysinfo(&info));
  CHECK(info.totalswap && info.freeswap <= info.totalswap);
  size_t pages = (size_t)(info.totalram * info.mem_unit / PAGE) + 16384;
  printf("ZRAM_BOOT: pressure working_set=%zu MiB physical_total=%lu MiB\n",
         pages / 256, info.totalram * info.mem_unit / (1024 * 1024));
  uint64_t before_out = metric("swapouts"), before_in = metric("swapins");
  uint64_t *p = mapping(pages);
  pthread_t fillers[8];
  struct fill_range ranges[8];
  for (unsigned i = 0; i < 8; i++) {
    ranges[i] = (struct fill_range){p, pages * i / 8, pages * (i + 1) / 8};
    CHECK(!pthread_create(&fillers[i], NULL, pressure_fill, &ranges[i]));
  }
  for (unsigned i = 0; i < 8; i++)
    CHECK(!pthread_join(fillers[i], NULL));
  CHECK(metric("swapouts") > before_out); /* no MADV_PAGEOUT in this phase */
  size_t stride = 257;
  while (gcd(stride, pages) != 1)
    stride += 2;
  size_t j = 0;
  for (size_t i = 0; i < pages; i++, j = (j + stride) % pages) {
    verify_page(p, j, 0x12345678, 0);
    p[j * 512] ^= 0x123456789abcdef0ULL;
  }
  for (size_t i = pages; i; i--)
    verify_page(p, i - 1, 0x12345678, 1);
  CHECK(metric("swapins") > before_in);
  uint64_t bytes = metric("memory_bytes"), limit = metric("memory_limit"),
           stored = metric("stored_pages");
  CHECK(bytes <= limit && stored > 0);
  printf("ZRAM_BOOT: automatic swapouts=%llu swapins=%llu stored=%llu "
         "memory=%llu limit=%llu\n",
         (unsigned long long)(metric("swapouts") - before_out),
         (unsigned long long)(metric("swapins") - before_in),
         (unsigned long long)stored, (unsigned long long)bytes,
         (unsigned long long)limit);
  CHECK(!munmap(p, pages * PAGE));
  CHECK(metric("stored_pages") < 256);
  puts("ZRAM_BOOT: PASS working set exceeds physical RAM by 64 MiB; all words "
       "verified in permuted and reverse order");
}
#ifndef EXPECT_DISABLED
#define EXPECT_DISABLED 0
#endif
#ifndef EXPECT_LIMIT
#define EXPECT_LIMIT 0
#endif
static void limited(void) {
  stage = "storage-limit exhaustion";
  size_t pages = 4096;
  uint64_t *p = mapping(pages);
  for (size_t j = 0; j < pages; j++) {
    uint64_t rng = j + 987;
    for (unsigned i = 0; i < 128; i++) {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      p[j * 512 + i] = rng;
    }
    for (unsigned i = 128; i < 512; i++)
      p[j * 512 + i] = p[j * 512 + i % 128];
  }
  uint64_t failures = metric("allocation_failures"),
           before = metric("swapouts");
  CHECK(!madvise(p, pages * PAGE, MADV_PAGEOUT));
  uint64_t out = metric("swapouts") - before;
  CHECK(out > 0 && out < pages && metric("allocation_failures") > failures);
  CHECK(metric("memory_bytes") <= metric("memory_limit"));
  for (size_t j = 0; j < pages; j++) {
    uint64_t rng = j + 987, pattern[128];
    for (unsigned i = 0; i < 128; i++) {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      pattern[i] = rng;
    }
    for (unsigned i = 0; i < 512; i++)
      CHECK(p[j * 512 + i] == pattern[i % 128]);
  }
  CHECK(!munmap(p, pages * PAGE));
  await_empty();
  puts("ZRAM_BOOT: PASS hard storage limit rejects further pageouts without "
       "losing a single byte or leaking slots");
}
int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "exec-child"))
    _exit(0);
  setvbuf(stdout, NULL, _IOLBF, 0);
  /* Native PID 1 auto-reaps dead init children. Exercise wait/fork from a
   * normal process so that init's reaper cannot consume our test statuses. */
  if (getpid() == 1) {
    pid_t worker = fork();
    CHECK(worker >= 0);
    if (worker)
      for (;;)
        pause();
  }
  puts("ZRAM_BOOT: START");
  mount("proc", "/proc", "proc", 0, NULL);
  mount("sysfs", "/sys", "sysfs", 0, NULL);
  if (!metric("enabled")) {
    CHECK(EXPECT_DISABLED);
    stage = "disabled";
    uint64_t *p = mapping(256);
    fill(p, 256, 10);
    CHECK(!madvise(p, 256 * PAGE, MADV_COLD));
    CHECK(!madvise(p, 256 * PAGE, MADV_PAGEOUT));
    verify(p, 256, 10);
    CHECK(!munmap(p, 256 * PAGE));
    CHECK(!metric("swapouts"));
    struct sysinfo info;
    CHECK(!sysinfo(&info) && !info.totalswap);
    puts("ZRAM_BOOT: PASS disabled mode preserves memory and reports no swap");
  } else {
    CHECK(!EXPECT_DISABLED);
    int fd = open("/dev/zram0", O_RDONLY);
    CHECK(fd >= 0);
    int ro = 0;
    CHECK(!ioctl(fd, 0x125e, &ro) && ro);
    close(fd);
    FILE *f = fopen("/sys/block/zram0/mm_stat", "r");
    CHECK(f);
    fclose(f);
    if (EXPECT_LIMIT)
      limited();
    else {
      lifecycle();
      parallel();
      pressure();
    }
  }
  puts("ZRAM_BOOT: PASS all tests");
  sync();
  reboot(RB_POWER_OFF);
  for (;;)
    pause();
}
