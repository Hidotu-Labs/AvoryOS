/*
 * nvme_test — raw block-device smoke/stress tool for AvoryOS.
 *
 * Subcommands:
 *   info
 *       List the kernel's block devices from /proc/partitions and sysfs.
 *   raw <device> [--mb=N] [--bytes=N] [--offset=BYTES] [--threads=N]
 *       [--skip-verify]
 *       Write a deterministic per-sector pattern over the region, then read
 *       it back and verify it.  The pattern is reproducible on the host:
 *       each 512-byte sector starts with its little-endian relative LBA and
 *       then repeats LE64(lba ^ 0xDEADBEEFCAFEBABE).
 *   hash <device> [--bytes=N] [--offset=BYTES]
 *       Print the SHA-256 of a device region as
 *       "NVME-HASH: dev=... offset=... bytes=... sha256=...".
 *   auto [--mb=N] [--seconds=S] [--loop=N]
 *       Headless mode used by the NVMe test init launcher: pick a scratch block
 *       device, run the Phase 3 transfer matrix (512 B .. 4 MiB at LBA
 *       0/mid/last plus pseudo-random offsets), multi-threaded passes, a
 *       SHA-256 report and an optional soak loop; print "NVME-TEST: PASS" or
 *       "NVME-TEST: FAIL", then power off.
 *       --verified-gib=N additionally runs verified full-device passes until
 *       N GiB have been checked, printing an NVME-VERIFIED total per pass so
 *       the host harness can accumulate the Phase 7 terabyte target.
 *
 * Exit status: 0 on PASS, 1 on FAIL.  auto always powers off.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define SECTOR_SIZE 512u
#define CHUNK_SECTORS 2048u /* 1 MiB */

#define MAX_PARTS 64
struct part_entry {
  char name[32];
  uint64_t bytes;
};
static struct part_entry parts[MAX_PARTS];
static int nparts;

static void list_dev_dir(void);
static uint64_t parse_u64(const char *s, uint64_t *out);

/*
 * Size of a block device in bytes.  /sys/class/block is authoritative (the
 * VFS stat path still carries a 32-bit length, so an 8 GiB namespace would
 * report 0); fall back to stat for kernels/images without sysfs.  The
 * returned testable size is capped below 4 GiB because sys_pread/pwrite64
 * truncate their offset to 32 bits until the phase 7 audit widens that.
 */
#define TESTABLE_MAX (0xFFFFFFFFULL - (16ULL * 1024 * 1024))

static uint64_t device_bytes(const char *path) {
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;

  char sysfs[128];
  snprintf(sysfs, sizeof(sysfs), "/sys/class/block/%.31s/size", name);
  int fd = open(sysfs, O_RDONLY);
  if (fd >= 0) {
    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
      uint64_t v = 0;
      if (parse_u64(buf, &v) && v > 0)
        return v;
    }
  }

  struct stat st;
  if (stat(path, &st) == 0 && st.st_size > 0)
    return (uint64_t)st.st_size;
  return 0;
}

static uint64_t parse_u64(const char *s, uint64_t *out) {
  uint64_t v = 0;
  if (!*s)
    return 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (uint64_t)(*s++ - '0');
  *out = v;
  return 1;
}

/* /proc/partitions: "major minor #blocks name" (1K blocks). */
static void load_partitions(void) {
  nparts = 0;
  int fd = open("/proc/partitions", O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "nvme_test: open /proc/partitions failed: %s\n",
            strerror(errno));
    return;
  }

  char buf[8192];
  int n = (int)read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    fprintf(stderr, "nvme_test: read /proc/partitions failed: n=%d %s\n", n,
            strerror(errno));
    return;
  }
  buf[n] = '\0';

  char *line = buf;
  int lineno = 0;
  while (*line) {
    char *end = line;
    while (*end && *end != '\n')
      end++;
    char saved = *end;
    *end = '\0';

    lineno++;
    if (lineno > 2 && nparts < MAX_PARTS) {
      char *p = line;
      while (*p == ' ')
        p++;
      while (*p >= '0' && *p <= '9')
        p++; /* major */
      while (*p == ' ')
        p++;
      while (*p >= '0' && *p <= '9')
        p++; /* minor */
      while (*p == ' ')
        p++;
      uint64_t blocks = 0;
      while (*p >= '0' && *p <= '9')
        blocks = blocks * 10 + (uint64_t)(*p++ - '0');
      while (*p == ' ')
        p++;
      int m = 0;
      while (*p && *p != ' ' && m < 31)
        parts[nparts].name[m++] = *p++;
      parts[nparts].name[m] = '\0';
      parts[nparts].bytes = blocks * 1024ULL;
      if (m > 0)
        nparts++;
    }

    if (saved == '\0')
      break;
    line = end + 1;
  }

  if (nparts == 0)
    fprintf(stderr, "nvme_test: /proc/partitions had no entries (n=%d, first "
                    "line: '%.*s')\n",
            n, 32, buf);
}

static void cmd_info(void) {
  load_partitions();
  printf("%-16s %14s  %s\n", "NAME", "SIZE", "PATH");
  if (nparts == 0) {
    printf("(no block devices found)\n");
    return;
  }
  for (int i = 0; i < nparts; i++) {
    uint64_t bytes = parts[i].bytes;
    char sysfs[128];
    snprintf(sysfs, sizeof(sysfs), "/sys/class/block/%.31s/size",
             parts[i].name);
    int fd = open(sysfs, O_RDONLY);
    if (fd >= 0) {
      char sbuf[32] = {0};
      int n = (int)read(fd, sbuf, sizeof(sbuf) - 1);
      close(fd);
      if (n > 0) {
        uint64_t v = 0;
        if (parse_u64(sbuf, &v))
          bytes = v;
      }
    }
    printf("%-16s %14llu  /dev/%s\n", parts[i].name,
           (unsigned long long)bytes, parts[i].name);
  }
  list_dev_dir();
}

/* Deterministic sector pattern ------------------------------------------- */

static void fill_sector(uint8_t *b, uint64_t lba) {
  uint64_t lo = lba;
  uint64_t hi = lba ^ 0xDEADBEEFCAFEBABEULL;
  for (int i = 0; i < 8; i++)
    b[i] = (uint8_t)(lo >> (8 * i));
  for (int i = 8; i < (int)SECTOR_SIZE; i++)
    b[i] = (uint8_t)(hi >> (8 * ((i - 8) & 7)));
}

static int check_sector(const uint8_t *b, uint64_t lba) {
  uint8_t expect[SECTOR_SIZE];
  fill_sector(expect, lba);
  return memcmp(b, expect, SECTOR_SIZE) == 0;
}

/* SHA-256 ------------------------------------------------------------------ */

struct sha256_ctx {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t data[64];
  size_t datalen;
};

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(struct sha256_ctx *ctx, const uint8_t *data) {
  uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];

  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
           ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^
                  (w[i - 15] >> 3);
    uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^
                  (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];
  f = ctx->state[5];
  g = ctx->state[6];
  h = ctx->state[7];

  for (int i = 0; i < 64; i++) {
    uint32_t s1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t s0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    t1 = h + s1 + ch + sha256_k[i] + w[i];
    t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

static void sha256_init(struct sha256_ctx *ctx) {
  ctx->datalen = 0;
  ctx->bitlen = 0;
  ctx->state[0] = 0x6a09e667;
  ctx->state[1] = 0xbb67ae85;
  ctx->state[2] = 0x3c6ef372;
  ctx->state[3] = 0xa54ff53a;
  ctx->state[4] = 0x510e527f;
  ctx->state[5] = 0x9b05688c;
  ctx->state[6] = 0x1f83d9ab;
  ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(struct sha256_ctx *ctx, const uint8_t *data,
                          size_t len) {
  for (size_t i = 0; i < len; i++) {
    ctx->data[ctx->datalen++] = data[i];
    if (ctx->datalen == 64) {
      sha256_transform(ctx, ctx->data);
      ctx->bitlen += 512;
      ctx->datalen = 0;
    }
  }
}

static void sha256_final(struct sha256_ctx *ctx, uint8_t hash[32]) {
  size_t i = ctx->datalen;

  ctx->data[i++] = 0x80;
  if (i > 56) {
    while (i < 64)
      ctx->data[i++] = 0;
    sha256_transform(ctx, ctx->data);
    i = 0;
  }
  while (i < 56)
    ctx->data[i++] = 0;
  ctx->bitlen += (uint64_t)ctx->datalen * 8;
  for (int b = 0; b < 8; b++)
    ctx->data[56 + b] = (uint8_t)(ctx->bitlen >> (56 - 8 * b));
  sha256_transform(ctx, ctx->data);

  for (i = 0; i < 8; i++)
    for (int b = 0; b < 4; b++)
      hash[i * 4 + b] = (uint8_t)(ctx->state[i] >> (24 - 8 * b));
}

/* Raw worker -------------------------------------------------------------- */

struct worker_arg {
  int fd;
  uint64_t start_sector;
  uint64_t sectors;
  int verify;
  int result; /* 0 ok, -1 write, -2 verify */
  uint64_t fail_sector;
};

static void *raw_worker(void *opaque) {
  struct worker_arg *a = (struct worker_arg *)opaque;
  uint8_t *buf = aligned_alloc(4096, CHUNK_SECTORS * SECTOR_SIZE);
  if (!buf) {
    a->result = -3;
    return NULL;
  }

  uint64_t done = 0;
  while (done < a->sectors) {
    uint64_t chunk = a->sectors - done;
    if (chunk > CHUNK_SECTORS)
      chunk = CHUNK_SECTORS;

    for (uint64_t i = 0; i < chunk; i++)
      fill_sector(buf + i * SECTOR_SIZE, a->start_sector + done + i);

    off_t off = (off_t)((a->start_sector + done) * SECTOR_SIZE);
    ssize_t wr = pwrite(a->fd, buf, (size_t)(chunk * SECTOR_SIZE), off);
    if (wr != (ssize_t)(chunk * SECTOR_SIZE)) {
      fprintf(stderr, "nvme_test: pwrite lba=%llu failed: %s\n",
              (unsigned long long)(a->start_sector + done), strerror(errno));
      a->result = -1;
      a->fail_sector = a->start_sector + done;
      free(buf);
      return NULL;
    }

    if (a->verify) {
      memset(buf, 0, (size_t)(chunk * SECTOR_SIZE));
      ssize_t rd = pread(a->fd, buf, (size_t)(chunk * SECTOR_SIZE), off);
      if (rd != (ssize_t)(chunk * SECTOR_SIZE)) {
        fprintf(stderr, "nvme_test: pread lba=%llu failed: %s\n",
                (unsigned long long)(a->start_sector + done), strerror(errno));
        a->result = -2;
        a->fail_sector = a->start_sector + done;
        free(buf);
        return NULL;
      }
      for (uint64_t i = 0; i < chunk; i++) {
        if (!check_sector(buf + i * SECTOR_SIZE, a->start_sector + done + i)) {
          fprintf(stderr, "nvme_test: verify mismatch at lba=%llu\n",
                  (unsigned long long)(a->start_sector + done + i));
          a->result = -2;
          a->fail_sector = a->start_sector + done + i;
          free(buf);
          return NULL;
        }
      }
    }

    done += chunk;
  }

  free(buf);
  a->result = 0;
  return NULL;
}

/* `bytes` must be a multiple of 512; `offset` must be 512-byte aligned. */
static int run_raw(const char *dev, uint64_t bytes, uint64_t offset,
                   int threads, int verify) {
  struct stat st;
  if (stat(dev, &st) != 0) {
    fprintf(stderr, "nvme_test: stat %s: %s\n", dev, strerror(errno));
    return 1;
  }
  if (bytes == 0 || bytes % SECTOR_SIZE != 0 || offset % SECTOR_SIZE != 0) {
    fprintf(stderr, "nvme_test: size and offset must be 512-byte multiples\n");
    return 1;
  }

  uint64_t total_sectors = bytes / SECTOR_SIZE;
  uint64_t dev_sectors =
      (uint64_t)st.st_size / SECTOR_SIZE; /* 0 when the node has no size */
  if (dev_sectors && offset / SECTOR_SIZE + total_sectors > dev_sectors) {
    fprintf(stderr,
            "nvme_test: requested %llu bytes at offset %llu past device size\n",
            (unsigned long long)bytes, (unsigned long long)offset);
    return 1;
  }
  if (threads < 1)
    threads = 1;
  if ((uint64_t)threads > total_sectors)
    threads = 1;

  printf("nvme_test: raw %s bytes=%llu offset=%llu threads=%d verify=%d\n", dev,
         (unsigned long long)bytes, (unsigned long long)offset, threads,
         verify);
  fflush(stdout);

  int fd = open(dev, O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "nvme_test: open %s: %s\n", dev, strerror(errno));
    return 1;
  }

  uint64_t per_thread = total_sectors / (uint64_t)threads;
  uint64_t start = offset / SECTOR_SIZE;

  pthread_t tids[64];
  struct worker_arg args[64];
  if (threads > 64) {
    threads = 64;
    per_thread = total_sectors / (uint64_t)threads;
  }

  for (int i = 0; i < threads; i++) {
    args[i].fd = fd;
    args[i].start_sector = start + (uint64_t)i * per_thread;
    args[i].sectors =
        (i == threads - 1) ? (total_sectors - (uint64_t)i * per_thread)
                           : per_thread;
    args[i].verify = verify;
    args[i].result = 0;
    args[i].fail_sector = 0;
    if (pthread_create(&tids[i], NULL, raw_worker, &args[i]) != 0) {
      fprintf(stderr, "nvme_test: pthread_create failed\n");
      close(fd);
      return 1;
    }
  }

  int rc = 0;
  for (int i = 0; i < threads; i++) {
    pthread_join(tids[i], NULL);
    if (args[i].result != 0) {
      fprintf(stderr, "nvme_test: worker %d failed (%d) at lba=%llu\n", i,
              args[i].result, (unsigned long long)args[i].fail_sector);
      rc = 1;
    }
  }

  fsync(fd);
  close(fd);

  if (rc == 0)
    printf("nvme_test: verified %llu bytes OK\n", (unsigned long long)bytes);
  return rc;
}

/* hash -------------------------------------------------------------------- */

static int run_hash(const char *dev, uint64_t bytes, uint64_t offset) {
  struct stat st;
  if (stat(dev, &st) != 0) {
    fprintf(stderr, "nvme_test: stat %s: %s\n", dev, strerror(errno));
    return 1;
  }
  if (bytes == 0 || bytes % SECTOR_SIZE != 0 || offset % SECTOR_SIZE != 0) {
    fprintf(stderr, "nvme_test: size and offset must be 512-byte multiples\n");
    return 1;
  }

  int fd = open(dev, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "nvme_test: open %s: %s\n", dev, strerror(errno));
    return 1;
  }

  struct sha256_ctx ctx;
  sha256_init(&ctx);
  uint8_t *buf = aligned_alloc(4096, CHUNK_SECTORS * SECTOR_SIZE);
  if (!buf) {
    fprintf(stderr, "nvme_test: hash buffer allocation failed\n");
    close(fd);
    return 1;
  }

  uint64_t remaining = bytes;
  uint64_t off = offset;
  int rc = 0;
  while (remaining > 0) {
    size_t chunk = remaining > CHUNK_SECTORS * SECTOR_SIZE
                       ? CHUNK_SECTORS * SECTOR_SIZE
                       : (size_t)remaining;
    ssize_t rd = pread(fd, buf, chunk, (off_t)off);
    if (rd != (ssize_t)chunk) {
      fprintf(stderr, "nvme_test: hash pread at %llu failed: %s\n",
              (unsigned long long)off, strerror(errno));
      rc = 1;
      break;
    }
    sha256_update(&ctx, buf, chunk);
    off += chunk;
    remaining -= chunk;
  }
  free(buf);
  close(fd);
  if (rc != 0)
    return rc;

  uint8_t digest[32];
  sha256_final(&ctx, digest);
  printf("NVME-HASH: dev=%s offset=%llu bytes=%llu sha256=", dev,
         (unsigned long long)offset, (unsigned long long)bytes);
  for (int i = 0; i < 32; i++)
    printf("%02x", digest[i]);
  printf("\n");
  fflush(stdout);
  return 0;
}

/* auto -------------------------------------------------------------------- */

static void power_off(void) {
  sync();
  syscall(SYS_reboot, 0xfee1deadu, 0x28121969u, 0x4321fedcu, 0);
  execl("/bin/shutdown", "shutdown", "-P", NULL);
}

/* Name scan: "nvme0n12" etc. is namespace 1 partition 2 of whichever
 * controller the test image landed on; sata02/sda2 are the AHCI equivalents. */
static int name_starts(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int name_ends(const char *s, const char *suffix) {
  size_t ls = strlen(s), lf = strlen(suffix);
  return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

/* Prefer a purpose-built scratch partition; fall back to whole devices.
 * "n12" is the historical NVMe naming (namespace 1, partition 2); "p2" is the
 * current one (nvme0n1p2). */
static const char *pick_scratch(void) {
  static char scratch_path[64];

  load_partitions();
  for (int i = 0; i < nparts; i++) {
    const char *name = parts[i].name;
    int preferred =
        (name_starts(name, "nvme") &&
         (name_ends(name, "n12") || name_ends(name, "p2"))) ||
        strcmp(name, "sata02") == 0 || strcmp(name, "sda2") == 0;
    if (preferred) {
      snprintf(scratch_path, sizeof(scratch_path), "/dev/%s", name);
      return scratch_path;
    }
  }

  static const char *candidates[] = {
      "/dev/nvme0n12",  /* test image partition 2 on NVMe (legacy naming) */
      "/dev/nvme0n1p2", /* test image partition 2 on NVMe (current naming) */
      "/dev/sata02",    /* test image partition 2 on AHCI */
      "/dev/sda2",      /* partition 2 with legacy naming */
      "/dev/sdb",       /* blank scratch image on the second disk */
      "/dev/nvme0n1",   /* whole NVMe namespace (blank scratch) */
      "/dev/sata0",     /* whole AHCI disk (last resort) */
  };
  int last_errno = 0;
  const char *last_path = NULL;
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    struct stat st;
    if (stat(candidates[i], &st) == 0)
      return candidates[i];
    last_errno = errno;
    last_path = candidates[i];
  }
  fprintf(stderr, "nvme_test: no scratch device (%s: %s)\n",
          last_path ? last_path : "?", strerror(last_errno));
  return NULL;
}

static void list_dev_dir(void) {
  DIR *dir = opendir("/dev");
  if (!dir) {
    fprintf(stderr, "nvme_test: opendir /dev failed: %s\n", strerror(errno));
    return;
  }
  printf("/dev entries:");
  struct dirent *ent;
  int count = 0;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_name[0] == '.' ||
        strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    printf(" %s", ent->d_name);
    if (++count > 64)
      break;
  }
  printf("\n");
  closedir(dir);
}

/*
 * Phase 3 transfer matrix: sizes from one sector to 4 MiB, at the start, the
 * middle and the end of the scratch device, plus a reproducible pseudo-random
 * offset per size.  Every write carries the absolute-LBA pattern, so passes
 * may overlap without invalidating earlier data.
 */
static int run_transfer_matrix(const char *dev, uint64_t dev_bytes,
                               uint64_t max_size, unsigned *passes) {
  static const uint64_t sizes[] = {512,   4096,    65536,   1048576,
                                   4194304, 8388608, 16777216};
  int failures = 0;
  unsigned count = 0;

  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    uint64_t size = sizes[i];
    if (size > dev_bytes || size > max_size)
      continue;

    uint64_t offsets[4];
    offsets[0] = 0;
    offsets[1] = ((dev_bytes / 2) / size) * size;
    offsets[2] = ((dev_bytes - size) / SECTOR_SIZE) * SECTOR_SIZE;
    /* Deterministic "random" offset: odd multiplier modulo the range. */
    uint64_t span = dev_bytes - size;
    offsets[3] = span ? ((size * 2654435761ULL) % (span / SECTOR_SIZE)) *
                            SECTOR_SIZE
                      : 0;

    for (int o = 0; o < 4; o++) {
      uint64_t off = offsets[o];
      int dup = 0;
      for (int p = 0; p < o; p++)
        if (offsets[p] == off)
          dup = 1;
      if (dup)
        continue;
      if (run_raw(dev, size, off, 1, 1) != 0)
        failures++;
      count++;
    }
  }

  *passes = count;
  return failures;
}

#define MAX_AUTO_DEVICES 8
#define CRASH_FILE "/crash-test.bin"
#define CRASH_MAGIC "AVCRSH01"
#define CRASH_CHUNK (64u * 1024u)

struct auto_opts {
  int big;    /* extend the matrix to 8/16 MiB */
  int deep;   /* 64-thread deep-queue pass */
  int fast;   /* skip everything except an explicitly requested mode */
  int pair;   /* compare two devices with identical patterns */
  int crash;  /* crash/fsync consistency mode */
  uint64_t crash_bytes;
  uint64_t verified_gib; /* cumulative verified GiB (Phase 7) */
  const char *devices;   /* comma-separated explicit list, or NULL */
};

static int parse_devices(const char *list, char paths[][64], int max) {
  int n = 0;
  const char *p = list;
  while (*p && n < max) {
    const char *comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    while (len > 0 && p[0] == ' ')
      p++, len--;
    while (len > 0 && p[len - 1] == ' ')
      len--;
    if (len == 0 || len >= 64)
      return -1;
    memcpy(paths[n], p, len);
    paths[n][len] = '\0';
    n++;
    if (!comma)
      break;
    p = comma + 1;
  }
  return n;
}

/* Matrix + multi-thread + hash on one device.  `deep` adds a 64-thread pass
 * so the depth-64 queue sees CID reuse and contention. */
static int test_device(const char *dev, int big, int deep) {
  uint64_t dev_bytes = device_bytes(dev);
  if (dev_bytes == 0) {
    fprintf(stderr, "nvme_test: cannot size %s\n", dev);
    return 1;
  }
  if (dev_bytes > TESTABLE_MAX)
    dev_bytes = TESTABLE_MAX;
  uint64_t max_size = big ? 16u * 1024 * 1024 : 4u * 1024 * 1024;
  unsigned passes = 0;
  int rc = 0;

  if (run_transfer_matrix(dev, dev_bytes, max_size, &passes) != 0)
    rc = 1;
  printf("NVME-MATRIX: %s dev=%s passes=%u\n", rc == 0 ? "PASS" : "FAIL", dev,
         passes);
  fflush(stdout);

  uint64_t mt_bytes =
      dev_bytes < 8u * 1024 * 1024 ? dev_bytes : 8u * 1024 * 1024;
  if (rc == 0) {
    if (run_raw(dev, mt_bytes, 0, 4, 1) != 0)
      rc = 1;
    if (rc == 0 && run_raw(dev, mt_bytes, 0, 8, 1) != 0)
      rc = 1;
  }
  printf("NVME-THREADS: %s dev=%s bytes=%llu\n", rc == 0 ? "PASS" : "FAIL", dev,
         (unsigned long long)mt_bytes);
  fflush(stdout);

  if (deep && rc == 0) {
    uint64_t deep_bytes =
        dev_bytes < 8u * 1024 * 1024 ? dev_bytes : 8u * 1024 * 1024;
    if (run_raw(dev, deep_bytes, 0, 64, 1) != 0)
      rc = 1;
    printf("NVME-DEEP: %s dev=%s threads=64 bytes=%llu\n",
           rc == 0 ? "PASS" : "FAIL", dev, (unsigned long long)deep_bytes);
    fflush(stdout);
  }

  uint64_t hash_bytes =
      dev_bytes < 4u * 1024 * 1024 ? dev_bytes : 4u * 1024 * 1024;
  if (rc == 0 && run_hash(dev, hash_bytes, 0) != 0)
    rc = 1;
  fflush(stdout);
  return rc;
}

/*
 * Pair mode: write the identical deterministic pattern to two devices (e.g.
 * a 512e and a 4Kn namespace) and verify the reads match each other.  The
 * host-side harness additionally byte-compares the two backing images.
 */
static int run_pair(const char *a, const char *b, uint64_t bytes) {
  uint64_t sa = device_bytes(a);
  uint64_t sb = device_bytes(b);
  if (sa == 0 || sb == 0) {
    fprintf(stderr, "nvme_test: pair device sizing failed\n");
    return 1;
  }
  uint64_t limit = sa < sb ? sa : sb;
  if (limit > TESTABLE_MAX)
    limit = TESTABLE_MAX;
  if (bytes > limit)
    bytes = limit;
  bytes = (bytes / SECTOR_SIZE) * SECTOR_SIZE;
  if (bytes == 0) {
    fprintf(stderr, "nvme_test: pair devices have no usable space\n");
    return 1;
  }

  int fda = open(a, O_RDWR);
  int fdb = open(b, O_RDWR);
  uint8_t *wbuf = aligned_alloc(4096, CHUNK_SECTORS * SECTOR_SIZE);
  uint8_t *rbuf = aligned_alloc(4096, CHUNK_SECTORS * SECTOR_SIZE);
  if (fda < 0 || fdb < 0 || !wbuf || !rbuf) {
    fprintf(stderr, "nvme_test: pair open/alloc failed\n");
    if (fda >= 0)
      close(fda);
    if (fdb >= 0)
      close(fdb);
    free(wbuf);
    free(rbuf);
    return 1;
  }

  int rc = 0;
  uint64_t done = 0;
  while (done < bytes) {
    uint64_t chunk = bytes - done;
    if (chunk > CHUNK_SECTORS * SECTOR_SIZE)
      chunk = CHUNK_SECTORS * SECTOR_SIZE;
    uint64_t sectors = chunk / SECTOR_SIZE;
    uint64_t base = done / SECTOR_SIZE;

    for (uint64_t i = 0; i < sectors; i++)
      fill_sector(wbuf + i * SECTOR_SIZE, base + i);
    if (pwrite(fda, wbuf, (size_t)chunk, (off_t)done) != (ssize_t)chunk ||
        pwrite(fdb, wbuf, (size_t)chunk, (off_t)done) != (ssize_t)chunk ||
        pread(fda, wbuf, (size_t)chunk, (off_t)done) != (ssize_t)chunk ||
        pread(fdb, rbuf, (size_t)chunk, (off_t)done) != (ssize_t)chunk ||
        memcmp(wbuf, rbuf, (size_t)chunk) != 0) {
      fprintf(stderr, "nvme_test: pair mismatch at offset %llu\n",
              (unsigned long long)done);
      rc = 1;
      break;
    }
    done += chunk;
  }

  fsync(fda);
  fsync(fdb);
  close(fda);
  close(fdb);
  free(wbuf);
  free(rbuf);
  printf("NVME-PAIR: %s devA=%s devB=%s bytes=%llu\n",
         rc == 0 ? "PASS" : "FAIL", a, b, (unsigned long long)bytes);
  fflush(stdout);
  return rc;
}

/* Crash/fsync consistency: verify the file left by the previous power cut,
 * fsync a fresh one, then reboot without syncing anything else. */
struct crash_header {
  char magic[8];
  uint64_t iter;
  uint64_t bytes;
  uint64_t seed;
} __attribute__((packed));

static uint64_t crash_seed_for(uint64_t iter, uint64_t bytes) {
  return 0x9E3779B97F4A7C15ULL * (iter + 1u) ^ (bytes << 1) ^ iter;
}

static int crash_verify(uint64_t *next_iter) {
  int fd = open(CRASH_FILE, O_RDONLY);
  if (fd < 0) {
    *next_iter = 1;
    return 0; /* first crash boot: nothing to verify */
  }

  struct crash_header hdr;
  if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
      memcmp(hdr.magic, CRASH_MAGIC, 8) != 0 || hdr.bytes < SECTOR_SIZE ||
      hdr.bytes > 64u * 1024 * 1024) {
    fprintf(stderr, "nvme_test: crash file header invalid\n");
    close(fd);
    *next_iter = 1;
    return 1;
  }

  uint8_t *buf = aligned_alloc(4096, SECTOR_SIZE);
  uint64_t sectors = hdr.bytes / SECTOR_SIZE;
  int rc = 0;
  if (!buf) {
    close(fd);
    return 1;
  }
  for (uint64_t i = 0; i < sectors; i++) {
    if (pread(fd, buf, SECTOR_SIZE, (off_t)(sizeof(hdr) + i * SECTOR_SIZE)) !=
            (ssize_t)SECTOR_SIZE ||
        !check_sector(buf, hdr.seed + i)) {
      fprintf(stderr, "nvme_test: crash verify mismatch at sector %llu\n",
              (unsigned long long)i);
      rc = 1;
      break;
    }
  }
  free(buf);
  close(fd);
  printf("NVME-CRASH-VERIFY: %s bytes=%llu iter=%llu\n",
         rc == 0 ? "PASS" : "FAIL", (unsigned long long)hdr.bytes,
         (unsigned long long)hdr.iter);
  fflush(stdout);
  *next_iter = hdr.iter + 1;
  return rc;
}

static int crash_write(uint64_t iter, uint64_t bytes) {
  int fd = open(CRASH_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fprintf(stderr, "nvme_test: cannot create %s: %s\n", CRASH_FILE,
            strerror(errno));
    return 1;
  }

  struct crash_header hdr;
  memcpy(hdr.magic, CRASH_MAGIC, 8);
  hdr.iter = iter;
  hdr.bytes = bytes;
  hdr.seed = crash_seed_for(iter, bytes);
  if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
    close(fd);
    return 1;
  }

  uint8_t *buf = aligned_alloc(4096, SECTOR_SIZE);
  int rc = 0;
  if (!buf) {
    close(fd);
    return 1;
  }
  uint64_t sectors = bytes / SECTOR_SIZE;
  for (uint64_t i = 0; i < sectors; i++) {
    fill_sector(buf, hdr.seed + i);
    if (write(fd, buf, SECTOR_SIZE) != (ssize_t)SECTOR_SIZE || fsync(fd) != 0) {
      fprintf(stderr, "nvme_test: crash write failed at sector %llu: %s\n",
              (unsigned long long)i, strerror(errno));
      rc = 1;
      break;
    }
  }
  free(buf);
  close(fd);
  printf("NVME-CRASH-WRITE: %s bytes=%llu iter=%llu\n",
         rc == 0 ? "PASS" : "FAIL", (unsigned long long)bytes,
         (unsigned long long)iter);
  fflush(stdout);
  return rc;
}

static int cmd_crash(uint64_t bytes) {
  if (bytes == 0)
    bytes = 256u * 1024u;
  bytes = (bytes / SECTOR_SIZE) * SECTOR_SIZE;

  uint64_t iter = 1;
  int rc = crash_verify(&iter);
  if (rc == 0)
    rc = crash_write(iter, bytes);

  if (rc != 0) {
    printf("NVME-TEST: FAIL\n");
    fflush(stdout);
    power_off();
    return 1;
  }

  /* Power cut: reboot straight away, with no sync() of the rest of the
   * system, so the fsync'd crash file is the only guaranteed state. */
  printf("NVME-TEST: PASS\n");
  fflush(stdout);
  syscall(SYS_reboot, 0xfee1deadu, 0x28121969u, 0x01234567u, 0);
  /* Fallback if the reboot syscall is unavailable. */
  printf("NVME-CRASH-REBOOT: fallback\n");
  fflush(stdout);
  power_off();
  return 0;
}

static int cmd_auto(uint64_t mb, uint64_t seconds, int loops,
                    const struct auto_opts *o) {
  if (o->crash)
    return cmd_crash(o->crash_bytes);

  cmd_info();

  char paths[MAX_AUTO_DEVICES][64];
  int ndev = 0;
  if (o->devices && *o->devices) {
    ndev = parse_devices(o->devices, paths, MAX_AUTO_DEVICES);
    if (ndev <= 0) {
      fprintf(stderr, "nvme_test: bad --devices list\n");
      ndev = 0;
    }
  } else {
    const char *dev = pick_scratch();
    if (dev) {
      snprintf(paths[0], sizeof(paths[0]), "%s", dev);
      ndev = 1;
    }
  }

  int rc = 0;
  if (ndev == 0) {
    fprintf(stderr, "nvme_test: no scratch device found\n");
    rc = 1;
  }

  if (rc == 0 && o->pair) {
    if (ndev < 2) {
      fprintf(stderr, "nvme_test: --pair needs two devices\n");
      rc = 1;
    } else if (run_pair(paths[0], paths[1], 16u * 1024 * 1024) != 0) {
      rc = 1;
    }
  }

  if (rc == 0 && !o->fast) {
    for (int i = 0; i < ndev; i++) {
      if (test_device(paths[i], o->big, o->deep) != 0)
        rc = 1;
      if (rc != 0)
        break;
    }
  }

  /* Optional soak: keep rewriting a 32 MiB (or device-sized) window. */
  if (rc == 0 && ndev > 0 && (seconds || loops)) {
    uint64_t dev_bytes = device_bytes(paths[0]);
    if (dev_bytes == 0) {
      rc = 1;
    } else {
      if (dev_bytes > TESTABLE_MAX)
        dev_bytes = TESTABLE_MAX;
      uint64_t soak_bytes =
          dev_bytes < 32u * 1024 * 1024 ? dev_bytes : 32u * 1024 * 1024;
      soak_bytes = (soak_bytes / SECTOR_SIZE) * SECTOR_SIZE;
      if (soak_bytes == 0)
        soak_bytes = SECTOR_SIZE;
      uint64_t start = (uint64_t)time(NULL);
      unsigned iterations = 0;
      while (1) {
        if (run_raw(paths[0], soak_bytes, 0, 1, 1) != 0) {
          rc = 1;
          break;
        }
        iterations++;
        if (loops && (int)iterations >= loops)
          break;
        if (seconds && (uint64_t)time(NULL) - start >= seconds)
          break;
      }
      printf("NVME-SOAK: %s iterations=%u seconds=%llu bytes=%llu\n",
             rc == 0 ? "PASS" : "FAIL", iterations,
             (unsigned long long)((uint64_t)time(NULL) - start),
             (unsigned long long)soak_bytes);
      fflush(stdout);
    }
  }

  /* Cumulative verified writes (Phase 7 acceptance): keep doing full verified
   * write+read passes until `verified_gib` GiB have been checked.  Each pass
   * prints a total the host harness adds to its 1 TiB ledger. */
  if (rc == 0 && ndev > 0 && o->verified_gib) {
    uint64_t dev_bytes = device_bytes(paths[0]);
    if (o->verified_gib > UINT64_MAX / (1024ull * 1024 * 1024)) {
      rc = 1;
    } else if (dev_bytes == 0) {
      fprintf(stderr, "nvme_test: cannot size %s for verified writes\n",
              paths[0]);
      rc = 1;
    } else {
      uint64_t target = o->verified_gib * 1024ull * 1024 * 1024;
      if (dev_bytes > TESTABLE_MAX)
        dev_bytes = TESTABLE_MAX;
      /* 256 MiB per pass keeps progress markers frequent and bounds the
       * per-pass allocation on multi-gigabyte devices. */
      uint64_t pass_bytes = dev_bytes < 256u * 1024 * 1024
                                ? dev_bytes
                                : 256u * 1024 * 1024;
      pass_bytes = (pass_bytes / SECTOR_SIZE) * SECTOR_SIZE;
      if (pass_bytes == 0)
        pass_bytes = SECTOR_SIZE;
      uint64_t total = 0;
      unsigned passes = 0;
      while (total < target) {
        if (run_raw(paths[0], pass_bytes, 0, 1, 1) != 0) {
          rc = 1;
          break;
        }
        total += pass_bytes;
        passes++;
        printf("NVME-VERIFIED: dev=%s pass=%u bytes=%llu total=%llu\n",
               paths[0], passes, (unsigned long long)pass_bytes,
               (unsigned long long)total);
        fflush(stdout);
      }
      if (rc == 0)
        printf("NVME-VERIFIED: PASS dev=%s gib=%llu passes=%u total=%llu\n",
               paths[0], (unsigned long long)o->verified_gib, passes,
               (unsigned long long)total);
      fflush(stdout);
    }
  }

  /* --mb is kept as an explicit user-requested size for the legacy harness
   * invocation; it re-runs a verified pass of that many MiB. */
  if (rc == 0 && ndev > 0 && mb) {
    uint64_t dev_bytes = device_bytes(paths[0]);
    uint64_t bytes = mb * 1024 * 1024;
    if (dev_bytes == 0 || bytes > dev_bytes ||
        run_raw(paths[0], bytes, 0, 1, 1) != 0)
      rc = 1;
  }

  /* Leave a stable marker for the host-side harness, then power off so QEMU
   * exits by itself and scripts/nvme/nvme-stress.sh never has to kill it. */
  if (rc == 0)
    printf("NVME-TEST: PASS\n");
  else
    printf("NVME-TEST: FAIL\n");
  fflush(stdout);
  power_off();
  return rc;
}

/* CLI --------------------------------------------------------------------- */

static void usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s info\n"
          "       %s raw <device> [--mb=N] [--bytes=N] [--offset=BYTES] "
          "[--threads=N] [--skip-verify]\n"
          "       %s hash <device> [--bytes=N] [--offset=BYTES]\n"
          "       %s auto [--mb=N] [--seconds=S] [--loop=N] "
          "[--devices=d1,d2,...]\n"
          "               [--big] [--deep] [--fast] [--pair] "
          "[--crash] [--crash-mb=N] [--verified-gib=N]\n",
          argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }

  if (strcmp(argv[1], "info") == 0) {
    cmd_info();
    return 0;
  }

  if (strcmp(argv[1], "auto") == 0) {
    uint64_t mb = 0;
    uint64_t seconds = 0;
    uint64_t loops = 0;
    struct auto_opts o;
    memset(&o, 0, sizeof(o));
    for (int i = 2; i < argc; i++) {
      uint64_t v = 0;
      if (strncmp(argv[i], "--mb=", 5) == 0 && parse_u64(argv[i] + 5, &v))
        mb = v;
      else if (strncmp(argv[i], "--seconds=", 10) == 0 &&
               parse_u64(argv[i] + 10, &v))
        seconds = v;
      else if (strncmp(argv[i], "--loop=", 7) == 0 &&
               parse_u64(argv[i] + 7, &v))
        loops = v;
      else if (strncmp(argv[i], "--devices=", 10) == 0)
        o.devices = argv[i] + 10;
      else if (strncmp(argv[i], "--crash-mb=", 11) == 0 &&
               parse_u64(argv[i] + 11, &v) && v)
        o.crash_bytes = v * 1024 * 1024;
      else if (strncmp(argv[i], "--verified-gib=", 15) == 0 &&
               parse_u64(argv[i] + 15, &v) && v)
        o.verified_gib = v;
      else if (strcmp(argv[i], "--big") == 0)
        o.big = 1;
      else if (strcmp(argv[i], "--deep") == 0)
        o.deep = 1;
      else if (strcmp(argv[i], "--fast") == 0)
        o.fast = 1;
      else if (strcmp(argv[i], "--pair") == 0)
        o.pair = 1;
      else if (strcmp(argv[i], "--crash") == 0)
        o.crash = 1;
      else {
        fprintf(stderr, "nvme_test: unknown auto option %s\n", argv[i]);
        return 2;
      }
    }
    return cmd_auto(mb, seconds, (int)loops, &o);
  }

  if (strcmp(argv[1], "hash") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 2;
    }
    const char *dev = argv[2];
    uint64_t bytes = 4u * 1024 * 1024;
    uint64_t offset = 0;
    for (int i = 3; i < argc; i++) {
      uint64_t v = 0;
      if (strncmp(argv[i], "--bytes=", 8) == 0 &&
          parse_u64(argv[i] + 8, &v))
        bytes = v;
      else if (strncmp(argv[i], "--offset=", 9) == 0 &&
               parse_u64(argv[i] + 9, &v))
        offset = v;
      else {
        fprintf(stderr, "nvme_test: unknown option %s\n", argv[i]);
        return 2;
      }
    }
    return run_hash(dev, bytes, offset);
  }

  if (strcmp(argv[1], "raw") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 2;
    }
    const char *dev = argv[2];
    uint64_t mb = 0;
    uint64_t bytes = 0;
    uint64_t offset = 0;
    int threads = 1;
    int verify = 1;
    for (int i = 3; i < argc; i++) {
      uint64_t v = 0;
      if (strncmp(argv[i], "--mb=", 5) == 0 && parse_u64(argv[i] + 5, &v) && v)
        mb = v;
      else if (strncmp(argv[i], "--bytes=", 8) == 0 &&
               parse_u64(argv[i] + 8, &v) && v)
        bytes = v;
      else if (strncmp(argv[i], "--offset=", 9) == 0 &&
               parse_u64(argv[i] + 9, &v))
        offset = v;
      else if (strncmp(argv[i], "--threads=", 10) == 0 &&
               parse_u64(argv[i] + 10, &v) && v)
        threads = (int)v;
      else if (strcmp(argv[i], "--skip-verify") == 0)
        verify = 0;
      else {
        fprintf(stderr, "nvme_test: unknown option %s\n", argv[i]);
        usage(argv[0]);
        return 2;
      }
    }
    if (bytes == 0)
      bytes = mb * 1024 * 1024;
    if (bytes == 0)
      bytes = 32u * 1024 * 1024;
    return run_raw(dev, bytes, offset, threads, verify);
  }

  usage(argv[0]);
  return 2;
}
