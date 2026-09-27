#include "fs/tmpfs.h"
#include "fs/vfs.h"
#include "console/klog.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"

static int g_tmpfs_fail = 0;

#define TASSERT(expr)                                        \
    do {                                                     \
        if (!(expr)) {                                       \
            klog_puts("  [tmpfs] FAIL: " #expr "\n");        \
            g_tmpfs_fail = 1;                                \
        }                                                    \
    } while (0)

#define PAGE_SIZE 4096

static tmpfs_sb_t *make_sb(uint64_t max_bytes, uint64_t max_inodes) {
    tmpfs_sb_t *sb = kmalloc(sizeof(tmpfs_sb_t));
    if (!sb)
        return NULL;
    sb->max_bytes   = max_bytes;
    sb->max_inodes  = max_inodes;
    sb->used_bytes  = 0;
    sb->used_inodes = 0;
    spinlock_init(&sb->lock);
    return sb;
}

static void test_basic_write_read(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create != NULL);
    TASSERT(root->create(root, "hello.txt", 0644) == 0);

    vfs_node_t *file = root->finddir(root, "hello.txt");
    TASSERT(file != NULL);
    if (!file)
        return;

    const char *msg  = "Hello, tmpfs!";
    uint32_t    mlen = (uint32_t)strlen(msg);

    uint32_t written = file->write(file, 0, mlen, (uint8_t *)msg);
    TASSERT(written == mlen);
    TASSERT(file->length == mlen);

    uint8_t rbuf[64];
    memset(rbuf, 0, sizeof(rbuf));
    uint32_t nread = file->read(file, 0, mlen, rbuf);
    TASSERT(nread == mlen);
    TASSERT(memcmp(rbuf, msg, mlen) == 0);
}

static void test_cross_page_write_read(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "big.bin", 0644) == 0);
    vfs_node_t *file = root->finddir(root, "big.bin");
    TASSERT(file != NULL);
    if (!file)
        return;

    uint32_t size = PAGE_SIZE * 3 + 512;
    uint8_t *wbuf = kmalloc(size);
    uint8_t *rbuf = kmalloc(size);
    TASSERT(wbuf != NULL && rbuf != NULL);
    if (!wbuf || !rbuf) {
        if (wbuf) kfree(wbuf);
        if (rbuf) kfree(rbuf);
        return;
    }

    for (uint32_t i = 0; i < size; i++)
        wbuf[i] = (uint8_t)(i & 0xFF);

    uint32_t written = file->write(file, 0, size, wbuf);
    TASSERT(written == size);
    TASSERT(file->length == size);

    memset(rbuf, 0, size);
    uint32_t nread = file->read(file, 0, size, rbuf);
    TASSERT(nread == size);
    TASSERT(memcmp(wbuf, rbuf, size) == 0);

    uint32_t mid_off = PAGE_SIZE - 16;
    uint32_t mid_len = 32;
    memset(rbuf, 0, mid_len);
    nread = file->read(file, mid_off, mid_len, rbuf);
    TASSERT(nread == mid_len);
    TASSERT(memcmp(rbuf, wbuf + mid_off, mid_len) == 0);

    kfree(wbuf);
    kfree(rbuf);
}

static void test_overwrite_partial(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "patch.bin", 0644) == 0);
    vfs_node_t *file = root->finddir(root, "patch.bin");
    TASSERT(file != NULL);
    if (!file)
        return;

    uint8_t initial[16];
    memset(initial, 0xAA, 16);
    TASSERT(file->write(file, 0, 16, initial) == 16);

    uint8_t patch[4] = {0x01, 0x02, 0x03, 0x04};
    TASSERT(file->write(file, 4, 4, patch) == 4);

    uint8_t result[16];
    TASSERT(file->read(file, 0, 16, result) == 16);
    TASSERT(result[0] == 0xAA);
    TASSERT(result[3] == 0xAA);
    TASSERT(result[4] == 0x01);
    TASSERT(result[5] == 0x02);
    TASSERT(result[6] == 0x03);
    TASSERT(result[7] == 0x04);
    TASSERT(result[8] == 0xAA);
}

static void test_truncate_shrink(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "shrink.bin", 0644) == 0);
    vfs_node_t *file = root->finddir(root, "shrink.bin");
    TASSERT(file != NULL);
    if (!file)
        return;

    uint32_t size = PAGE_SIZE * 2 + 256;
    uint8_t *buf  = kmalloc(size);
    TASSERT(buf != NULL);
    if (!buf)
        return;

    memset(buf, 0x55, size);
    TASSERT(file->write(file, 0, size, buf) == size);
    TASSERT(file->length == size);
    kfree(buf);

    uint64_t bytes_before = sb->used_bytes;
    TASSERT(file->truncate(file, 128) == 0);
    TASSERT(file->length == 128);
    TASSERT(sb->used_bytes < bytes_before);

    uint8_t rbuf[256];
    uint32_t nread = file->read(file, 0, 256, rbuf);
    TASSERT(nread == 128);

    uint8_t check[128];
    memset(check, 0x55, 128);
    TASSERT(memcmp(rbuf, check, 128) == 0);
}

static void test_truncate_grow(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "grow.bin", 0644) == 0);
    vfs_node_t *file = root->finddir(root, "grow.bin");
    TASSERT(file != NULL);
    if (!file)
        return;

    uint8_t seed[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    TASSERT(file->write(file, 0, 4, seed) == 4);

    uint32_t new_size = PAGE_SIZE + 512;
    TASSERT(file->truncate(file, new_size) == 0);
    TASSERT(file->length == new_size);

    uint8_t rbuf[4];
    TASSERT(file->read(file, 0, 4, rbuf) == 4);
    TASSERT(memcmp(rbuf, seed, 4) == 0);

    uint8_t zero_check[512];
    memset(zero_check, 0, 512);
    uint8_t tail[512];
    TASSERT(file->read(file, PAGE_SIZE, 512, tail) == 512);
    TASSERT(memcmp(tail, zero_check, 512) == 0);
}

static void test_truncate_to_zero(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "zero.bin", 0644) == 0);
    vfs_node_t *file = root->finddir(root, "zero.bin");
    TASSERT(file != NULL);
    if (!file)
        return;

    uint8_t data[PAGE_SIZE * 2];
    memset(data, 0xFF, sizeof(data));
    TASSERT(file->write(file, 0, sizeof(data), data) == sizeof(data));
    TASSERT(sb->used_bytes >= PAGE_SIZE * 2);

    uint64_t bytes_before = sb->used_bytes;
    TASSERT(file->truncate(file, 0) == 0);
    TASSERT(file->length == 0);
    TASSERT(sb->used_bytes < bytes_before);
    TASSERT(file->read(file, 0, 1, data) == 0);
}

static void test_unlink(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "bye.txt", 0644) == 0);
    TASSERT(root->finddir(root, "bye.txt") != NULL);

    vfs_node_t *f = root->finddir(root, "bye.txt");
    uint8_t filler[PAGE_SIZE];
    memset(filler, 0xCC, sizeof(filler));
    f->write(f, 0, sizeof(filler), filler);
    TASSERT(sb->used_bytes >= PAGE_SIZE);

    uint64_t inodes_before = sb->used_inodes;
    uint64_t bytes_before  = sb->used_bytes;
    TASSERT(root->unlink(root, "bye.txt") == 0);
    TASSERT(root->finddir(root, "bye.txt") == NULL);
    TASSERT(sb->used_inodes < inodes_before);
    TASSERT(sb->used_bytes < bytes_before);
}

static void test_mkdir_and_rmdir(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->mkdir(root, "subdir", 0755) == 0);
    vfs_node_t *sub = root->finddir(root, "subdir");
    TASSERT(sub != NULL);
    TASSERT((sub->flags & FS_TYPE_MASK) == FS_DIRECTORY);

    TASSERT(sub->create(sub, "inner.txt", 0644) == 0);
    TASSERT(sub->finddir(sub, "inner.txt") != NULL);

    TASSERT(root->rmdir(root, "subdir") == -1);

    TASSERT(sub->unlink(sub, "inner.txt") == 0);
    TASSERT(root->rmdir(root, "subdir") == 0);
    TASSERT(root->finddir(root, "subdir") == NULL);
}

static void test_rename(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "old.txt", 0644) == 0);
    vfs_node_t *f = root->finddir(root, "old.txt");
    TASSERT(f != NULL);

    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    f->write(f, 0, 8, payload);

    TASSERT(root->rename(root, "old.txt", "new.txt") == 0);
    TASSERT(root->finddir(root, "old.txt") == NULL);

    vfs_node_t *renamed = root->finddir(root, "new.txt");
    TASSERT(renamed != NULL);
    TASSERT(renamed->length == 8);

    uint8_t rbuf[8];
    TASSERT(renamed->read(renamed, 0, 8, rbuf) == 8);
    TASSERT(memcmp(rbuf, payload, 8) == 0);
}

static void test_rename_overwrite(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "src.txt", 0644) == 0);
    TASSERT(root->create(root, "dst.txt", 0644) == 0);

    vfs_node_t *src = root->finddir(root, "src.txt");
    uint8_t data[4] = {0xAB, 0xCD, 0xEF, 0x01};
    src->write(src, 0, 4, data);

    TASSERT(root->rename(root, "src.txt", "dst.txt") == 0);
    TASSERT(root->finddir(root, "src.txt") == NULL);

    vfs_node_t *dst = root->finddir(root, "dst.txt");
    TASSERT(dst != NULL);
    uint8_t rbuf[4];
    TASSERT(dst->read(dst, 0, 4, rbuf) == 4);
    TASSERT(memcmp(rbuf, data, 4) == 0);
}

static void test_symlink(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->symlink(root, "link.txt", "/tmp/target.txt") == 0);
    vfs_node_t *sl = root->finddir(root, "link.txt");
    TASSERT(sl != NULL);
    TASSERT((sl->flags & FS_TYPE_MASK) == FS_SYMLINK);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    int len = sl->readlink(sl, buf, sizeof(buf));
    TASSERT(len > 0);
    TASSERT(strcmp(buf, "/tmp/target.txt") == 0);

    TASSERT(root->unlink(root, "link.txt") == 0);
    TASSERT(root->finddir(root, "link.txt") == NULL);
}

static void test_readdir(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "a.txt", 0644) == 0);
    TASSERT(root->create(root, "b.txt", 0644) == 0);
    TASSERT(root->mkdir(root, "c_dir", 0755) == 0);

    struct dirent *de;

    de = root->readdir(root, 0);
    TASSERT(de != NULL);
    TASSERT(strcmp(de->name, ".") == 0);

    de = root->readdir(root, 1);
    TASSERT(de != NULL);
    TASSERT(strcmp(de->name, "..") == 0);

    int found_a = 0, found_b = 0, found_c = 0;
    for (uint32_t i = 2; ; i++) {
        de = root->readdir(root, i);
        if (!de)
            break;
        if (strcmp(de->name, "a.txt") == 0) found_a = 1;
        if (strcmp(de->name, "b.txt") == 0) found_b = 1;
        if (strcmp(de->name, "c_dir") == 0) found_c = 1;
    }
    TASSERT(found_a && found_b && found_c);
}

static void test_statfs(void) {
    tmpfs_sb_t *sb = make_sb(TMPFS_DEFAULT_MAX_BYTES, TMPFS_DEFAULT_MAX_INODES);
    TASSERT(sb != NULL);
    if (!sb)
        return;

    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root) {
        kfree(sb);
        return;
    }

    struct statfs_buf buf;
    memset(&buf, 0, sizeof(buf));

    int r = root->statfs(root, &buf);
    TASSERT(r == 0);
    TASSERT(buf.f_type == TMPFS_MAGIC);
    TASSERT(buf.f_bsize == PAGE_SIZE);
    TASSERT(buf.f_blocks == (int64_t)(sb->max_bytes / PAGE_SIZE));
    TASSERT(buf.f_bfree  == buf.f_blocks);
    TASSERT(buf.f_bavail == buf.f_blocks);
    TASSERT(buf.f_files  == (int64_t)sb->max_inodes);
    TASSERT(buf.f_namelen == 255);

    TASSERT(root->create(root, "stat_probe.bin", 0644) == 0);
    vfs_node_t *f = root->finddir(root, "stat_probe.bin");
    uint8_t pg[PAGE_SIZE];
    memset(pg, 1, sizeof(pg));
    f->write(f, 0, sizeof(pg), pg);

    memset(&buf, 0, sizeof(buf));
    r = root->statfs(root, &buf);
    TASSERT(r == 0);
    TASSERT(buf.f_bfree < buf.f_blocks);
}

static void test_inode_quota(void) {
    tmpfs_sb_t *sb = make_sb(64 * 1024 * 1024, 4);
    TASSERT(sb != NULL);
    if (!sb)
        return;

    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root) {
        kfree(sb);
        return;
    }

    TASSERT(root->create(root, "f1.txt", 0644) == 0);
    TASSERT(root->create(root, "f2.txt", 0644) == 0);
    TASSERT(root->create(root, "f3.txt", 0644) == 0);

    TASSERT(root->create(root, "f4.txt", 0644) != 0);
    TASSERT(root->finddir(root, "f4.txt") == NULL);

    root->unlink(root, "f1.txt");
    TASSERT(root->create(root, "f4.txt", 0644) == 0);
    TASSERT(root->finddir(root, "f4.txt") != NULL);
}

static void test_byte_quota(void) {
    uint64_t max_bytes = PAGE_SIZE * 2;
    tmpfs_sb_t *sb = make_sb(max_bytes, 64);
    TASSERT(sb != NULL);
    if (!sb)
        return;

    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root) {
        kfree(sb);
        return;
    }

    TASSERT(root->create(root, "quota_a.bin", 0644) == 0);
    TASSERT(root->create(root, "quota_b.bin", 0644) == 0);

    vfs_node_t *fa = root->finddir(root, "quota_a.bin");
    vfs_node_t *fb = root->finddir(root, "quota_b.bin");
    TASSERT(fa != NULL && fb != NULL);

    uint8_t pg[PAGE_SIZE];
    memset(pg, 0xBB, sizeof(pg));

    TASSERT(fa->write(fa, 0, PAGE_SIZE, pg) == PAGE_SIZE);
    TASSERT(fb->write(fb, 0, PAGE_SIZE, pg) == PAGE_SIZE);

    uint32_t written = fb->write(fb, PAGE_SIZE, PAGE_SIZE, pg);
    TASSERT(written == 0);
}

static void test_write_at_page_boundary(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "boundary.bin", 0644) == 0);
    vfs_node_t *f = root->finddir(root, "boundary.bin");
    TASSERT(f != NULL);
    if (!f)
        return;

    uint8_t before[8];
    uint8_t after[8];
    memset(before, 0xBB, 8);
    memset(after,  0xCC, 8);

    TASSERT(f->write(f, PAGE_SIZE - 8, 8, before) == 8);
    TASSERT(f->write(f, PAGE_SIZE,     8, after)  == 8);
    TASSERT(f->length == PAGE_SIZE + 8);

    uint8_t rbuf[16];
    TASSERT(f->read(f, PAGE_SIZE - 8, 16, rbuf) == 16);
    TASSERT(memcmp(rbuf,     before, 8) == 0);
    TASSERT(memcmp(rbuf + 8, after,  8) == 0);
}

static void test_chmod_chown(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "perms.txt", 0644) == 0);
    vfs_node_t *f = root->finddir(root, "perms.txt");
    TASSERT(f != NULL);
    if (!f)
        return;

    TASSERT(f->chmod(f, 0755) == 0);
    TASSERT((f->mask & 0777) == 0755);

    TASSERT(f->chown(f, 1000, 1000) == 0);
    TASSERT(f->uid == 1000);
    TASSERT(f->gid == 1000);
}

static void test_sparse_read(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "sparse.bin", 0644) == 0);
    vfs_node_t *f = root->finddir(root, "sparse.bin");
    TASSERT(f != NULL);
    if (!f)
        return;

    uint8_t tail[4] = {0xF0, 0xF1, 0xF2, 0xF3};
    TASSERT(f->write(f, PAGE_SIZE * 2, 4, tail) == 4);
    TASSERT(f->length == PAGE_SIZE * 2 + 4);

    uint8_t rbuf[PAGE_SIZE];
    uint32_t nread = f->read(f, 0, PAGE_SIZE, rbuf);
    TASSERT(nread == PAGE_SIZE);
    for (uint32_t i = 0; i < PAGE_SIZE; i++)
        TASSERT(rbuf[i] == 0);

    uint8_t tail_read[4];
    TASSERT(f->read(f, PAGE_SIZE * 2, 4, tail_read) == 4);
    TASSERT(memcmp(tail_read, tail, 4) == 0);
}

static void test_read_past_eof(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_create_root(sb);
    TASSERT(root != NULL);
    if (!root)
        return;

    TASSERT(root->create(root, "eof.txt", 0644) == 0);
    vfs_node_t *f = root->finddir(root, "eof.txt");
    TASSERT(f != NULL);
    if (!f)
        return;

    uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    TASSERT(f->write(f, 0, 16, data) == 16);

    uint8_t rbuf[32];
    memset(rbuf, 0, sizeof(rbuf));
    uint32_t nread = f->read(f, 8, 32, rbuf);
    TASSERT(nread == 8);
    TASSERT(memcmp(rbuf, data + 8, 8) == 0);

    nread = f->read(f, 100, 8, rbuf);
    TASSERT(nread == 0);
}

void tmpfs_test(void) {
    klog_puts(KLOG_CLR_CYAN
              "[TEST ] tmpfs — PMM-backed tmpfs unit tests\n"
              KLOG_CLR_RESET);

    tmpfs_sb_t *sb = make_sb(TMPFS_DEFAULT_MAX_BYTES, TMPFS_DEFAULT_MAX_INODES);
    if (!sb) {
        klog_puts(KLOG_CLR_RED
                  "[ FAIL] tmpfs: could not allocate superblock\n"
                  KLOG_CLR_RESET);
        return;
    }

    test_basic_write_read(sb);
    test_cross_page_write_read(sb);
    test_overwrite_partial(sb);
    test_truncate_shrink(sb);
    test_truncate_grow(sb);
    test_truncate_to_zero(sb);
    test_unlink(sb);
    test_mkdir_and_rmdir(sb);
    test_rename(sb);
    test_rename_overwrite(sb);
    test_symlink(sb);
    test_readdir(sb);
    test_statfs();
    test_write_at_page_boundary(sb);
    test_chmod_chown(sb);
    test_sparse_read(sb);
    test_read_past_eof(sb);

    test_inode_quota();
    test_byte_quota();

    if (g_tmpfs_fail) {
        klog_puts(KLOG_CLR_RED
                  "[ FAIL] tmpfs unit tests — see failures above\n"
                  KLOG_CLR_RESET);
    } else {
        klog_puts(KLOG_CLR_GREEN
                  "[  OK ] tmpfs unit tests PASSED\n"
                  KLOG_CLR_RESET);
    }
}
