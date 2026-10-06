#define _GNU_SOURCE

/*
 * APM - small Alpine APK package manager for Linux and AscentOS.
 *
 * Design goals:
 *   - no dependency on apk-tools
 *   - use only POSIX libc + curl/gzip/tar utilities
 *   - never extract directly into / while parsing an APK
 *   - unpack into a staging directory first
 *   - overwrite existing files deliberately
 *   - ignore Alpine package metadata that an OS may not support
 *   - keep an installed-file manifest for safe removal
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APM_ROOT      "/etc/apm"
#define APM_CACHE     APM_ROOT "/cache"
#define APM_INSTALLED APM_ROOT "/installed"
#define APM_TMP       "/tmp/apm"
#define ALPINE_CDN    "https://dl-cdn.alpinelinux.org/alpine"
#define ALPINE_ARCH   "x86_64"
#define DEFAULT_BRANCH "v3.21"

struct repo {
    const char *branch;
    const char *name;
};

static const struct repo repos[] = {
    {"v3.21", "main"},
    {"v3.21", "community"},
    {"edge", "main"},
    {"edge", "community"},
    {"edge", "testing"},
    {NULL, NULL}
};

typedef struct {
    char name[256];
    char version[256];
    char arch[64];
    char desc[1024];
    char url[1024];
    char license[128];
    char deps[2048];
    char size[64];
    char isize[64];
    char branch[64];
    char repo[64];
} Package;

typedef int (*package_cb)(const Package *, void *);

static int command(const char *cmd)
{
    int rc = system(cmd);
    if (rc == -1)
        return -1;
    return rc;
}

static int command_ok(const char *cmd)
{
    return command(cmd) == 0;
}

static int mkdir_one(const char *path, mode_t mode)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(path, mode) == 0)
        return 0;
    return errno == EEXIST ? 0 : -1;
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    size_t n;

    if (!path || !*path)
        return -1;

    n = strlen(path);
    if (n >= sizeof(tmp))
        return -1;

    memcpy(tmp, path, n + 1);

    if (tmp[0] == '/') {
        char *p = tmp + 1;
        while (*p) {
            if (*p == '/') {
                *p = '\0';
                if (mkdir_one(tmp, mode) < 0)
                    return -1;
                *p = '/';
            }
            p++;
        }
    } else {
        char *p = tmp;
        while (*p) {
            if (*p == '/') {
                *p = '\0';
                if (*tmp && mkdir_one(tmp, mode) < 0)
                    return -1;
                *p = '/';
            }
            p++;
        }
    }

    return mkdir_one(tmp, mode);
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int read_all(const char *path, unsigned char **out, size_t *out_len)
{
    int fd;
    unsigned char *buf = NULL;
    size_t cap = 65536, len = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    buf = malloc(cap);
    if (!buf) {
        close(fd);
        return -1;
    }

    for (;;) {
        if (len == cap) {
            size_t newcap = cap * 2;
            unsigned char *nb;
            if (newcap < cap) {
                free(buf);
                close(fd);
                return -1;
            }
            nb = realloc(buf, newcap);
            if (!nb) {
                free(buf);
                close(fd);
                return -1;
            }
            buf = nb;
            cap = newcap;
        }

        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }

    close(fd);
    *out = buf;
    *out_len = len;
    return 0;
}

static int write_all_fd(int fd, const unsigned char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 65536)
            chunk = 65536;
        ssize_t n = write(fd, buf + off, chunk);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static void index_path(const char *branch, const char *repo,
                       char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s/index-%s-%s", APM_CACHE, branch, repo);
}

static int fetch_index(const char *branch, const char *repo)
{
    char url[PATH_MAX], archive[PATH_MAX], staging[PATH_MAX];
    char cmd[PATH_MAX * 2];

    snprintf(url, sizeof(url), "%s/%s/%s/%s/APKINDEX.tar.gz",
             ALPINE_CDN, branch, repo, ALPINE_ARCH);
    snprintf(archive, sizeof(archive), "%s/index-%s-%s.tar.gz",
             APM_TMP, branch, repo);
    snprintf(staging, sizeof(staging), "%s/index-%s-%s", APM_TMP, branch, repo);

    if (!command_ok("mkdir -p /etc/apm/cache /tmp/apm")) {
        printf("[apm] cannot create cache directories: %s\n", strerror(errno));
        fflush(stdout);
        return -1;
    }

    /* Retry the download up to 3 times — first HTTPS connection can fail
     * transiently while the TCP/TLS stack warms up on AscentOS. */
    int download_ok = 0;
    for (int attempt = 1; attempt <= 3; attempt++) {
        snprintf(cmd, sizeof(cmd),
                 "curl -fL --show-error --retry 2 '%s' -o '%s'",
                 url, archive);
        if (command_ok(cmd)) {
            download_ok = 1;
            break;
        }
        printf("[apm] download attempt %d/3 failed for %s/%s\n",
               attempt, branch, repo);
        fflush(stdout);
        unlink(archive);
    }
    if (!download_ok) {
        printf("[apm] all download attempts failed for %s/%s\n", branch, repo);
        fflush(stdout);
        return -1;
    }

    /* Run rm, mkdir-p, and tar as separate steps so failures are visible. */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", staging);
    if (!command_ok(cmd)) {
        /* Non-fatal: directory may not exist yet */
    }

    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", staging);
    if (!command_ok(cmd)) {
        printf("[apm] cannot create staging dir for %s/%s\n", branch, repo);
        fflush(stdout);
        unlink(archive);
        return -1;
    }

    snprintf(cmd, sizeof(cmd),
             "tar -xzf '%s' -C '%s' --no-same-owner --no-same-permissions --touch",
             archive, staging);
    if (!command_ok(cmd)) {
        printf("[apm] failed to extract APKINDEX for %s/%s\n", branch, repo);
        fflush(stdout);
        unlink(archive);
        return -1;
    }

    char source[PATH_MAX], destination[PATH_MAX];
    snprintf(source, sizeof(source), "%s/APKINDEX", staging);
    index_path(branch, repo, destination, sizeof(destination));

    if (!file_exists(source)) {
        printf("[apm] APKINDEX missing after extraction for %s/%s\n", branch, repo);
        fflush(stdout);
        unlink(archive);
        return -1;
    }

    int in = open(source, O_RDONLY);
    int out = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) {
        printf("[apm] cannot open index files for %s/%s: %s\n",
               branch, repo, strerror(errno));
        fflush(stdout);
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        unlink(archive);
        return -1;
    }

    unsigned char buf[65536];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(in); close(out); unlink(archive); return -1;
        }
        if (n == 0) break;
        if (write_all_fd(out, buf, (size_t)n) < 0) {
            close(in); close(out); unlink(archive); return -1;
        }
    }
    close(in);
    close(out);
    unlink(archive);

    printf("[apm] updated %s/%s\n", branch, repo);
    return 0;
}

static void package_init(Package *p, const char *branch, const char *repo)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->branch, sizeof(p->branch), "%s", branch);
    snprintf(p->repo, sizeof(p->repo), "%s", repo);
}

static void field_copy(char *dst, size_t dst_sz, const char *value, size_t len)
{
    while (len > 0 && (value[len - 1] == '\r' || value[len - 1] == '\n'))
        len--;
    if (len >= dst_sz)
        len = dst_sz - 1;
    memcpy(dst, value, len);
    dst[len] = '\0';
}

static int parse_record(const char *record, size_t len, const char *branch,
                        const char *repo, Package *p)
{
    package_init(p, branch, repo);

    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && record[end] != '\n') end++;

        if (end >= pos + 2 && record[pos + 1] == ':') {
            char tag = record[pos];
            const char *value = record + pos + 2;
            size_t vlen = end - (pos + 2);

            switch (tag) {
            case 'P': field_copy(p->name, sizeof(p->name), value, vlen); break;
            case 'V': field_copy(p->version, sizeof(p->version), value, vlen); break;
            case 'A': field_copy(p->arch, sizeof(p->arch), value, vlen); break;
            case 'T': field_copy(p->desc, sizeof(p->desc), value, vlen); break;
            case 'U': field_copy(p->url, sizeof(p->url), value, vlen); break;
            case 'L': field_copy(p->license, sizeof(p->license), value, vlen); break;
            case 'D': field_copy(p->deps, sizeof(p->deps), value, vlen); break;
            case 'S': field_copy(p->size, sizeof(p->size), value, vlen); break;
            case 'I': field_copy(p->isize, sizeof(p->isize), value, vlen); break;
            default: break;
            }
        }

        pos = end < len ? end + 1 : end;
    }

    return p->name[0] != '\0';
}

static int foreach_index(const char *branch, const char *repo,
                         package_cb cb, void *ctx)
{
    char path[PATH_MAX];
    unsigned char *buf = NULL;
    size_t len = 0;

    index_path(branch, repo, path, sizeof(path));
    if (read_all(path, &buf, &len) < 0) {
        printf("[apm debug] read_all failed for %s\n", path);
        fflush(stdout);
        return 0;
    }
    printf("[apm debug] index %s/%s read %zu bytes\n", branch, repo, len);
    fflush(stdout);

    size_t start = 0;
    size_t record_count = 0;
    while (start < len) {
        /* Skip leading whitespace/newlines */
        while (start < len && (buf[start] == '\n' || buf[start] == '\r'))
            start++;
        if (start >= len) break;

        /* Find end of record (blank line: \n\n or \n\r\n) */
        size_t end = start;
        while (end < len) {
            if (buf[end] == '\n') {
                size_t next = end + 1;
                if (next < len && buf[next] == '\r') next++;
                if (next < len && buf[next] == '\n') {
                    break;
                }
            }
            end++;
        }

        size_t record_len = end - start;
        if (record_len) {
            Package p;
            if (parse_record((const char *)buf + start, record_len,
                             branch, repo, &p)) {
                record_count++;
                if (cb(&p, ctx)) {
                    free(buf);
                    return 1;
                }
            }
        }

        /* Advance start past the blank line separator */
        start = end;
        while (start < len && (buf[start] == '\n' || buf[start] == '\r'))
            start++;
    }
    printf("[apm debug] %s/%s parsed %zu records\n", branch, repo, record_count);
    fflush(stdout);

    free(buf);
    return 0;
}

struct lookup {
    const char *name;
    Package pkg;
    int found;
};

static int lookup_cb(const Package *p, void *arg)
{
    struct lookup *l = arg;
    if (strcmp(p->name, l->name) != 0)
        return 0;
    l->pkg = *p;
    l->found = 1;
    return 1;
}

static int find_package(const char *name, Package *out)
{
    struct lookup l;
    memset(&l, 0, sizeof(l));
    l.name = name;

    for (int i = 0; repos[i].branch; ++i) {
        if (foreach_index(repos[i].branch, repos[i].name, lookup_cb, &l)) {
            *out = l.pkg;
            return 0;
        }
    }
    printf("[apm debug] searched all repos for '%s', not found\n", name);
    fflush(stdout);
    return -1;
}

static int install_path_safe(const char *name)
{
    /* Package names are never used as filesystem paths except for metadata. */
    if (!name || !*name)
        return 0;
    for (const char *p = name; *p; ++p) {
        if (!(('a' <= *p && *p <= 'z') ||
              ('A' <= *p && *p <= 'Z') ||
              ('0' <= *p && *p <= '9') ||
              *p == '+' || *p == '.' || *p == '-' || *p == '_'))
            return 0;
    }
    return 1;
}

static int extract_data_stream(const char *apk, const char *tar_path)
{
    /*
     * Modern Alpine APKs are a single gzip-compressed tarball containing
     * .SIGN.*, .PKGINFO and the payload (verified with xeyes-1.3.0-r0.apk:
     * one gzip member, `gzip -dc` -> tar with .SIGN/.PKGINFO/usr/...).
     * Older APKs are 3 concatenated gzip streams (signature + control +
     * data); `gzip -dc` concatenates them and `tar --ignore-zeros` skips
     * the inter-stream padding (see scripts/setup-arch.sh).
     *
     * The previous implementation brute-forced every 0x1f8b offset from the
     * end and validated each slice. Deflate data routinely contains bytes
     * that look like gzip headers, so it picked truncated inner slices
     * (missing leading members) on GNU toolchains and failed entirely on
     * busybox (stricter gzip/tar) with "could not locate a valid APK data
     * tar stream". It was also O(n^2) I/O: an 8MB package means millions
     * of slice+fork+exec iterations.
     *
     * Fix: decompress once from offset 0, then validate as tar.
     */
    char cmd[PATH_MAX * 2];

    unlink(tar_path);

    snprintf(cmd, sizeof(cmd), "gzip -dc '%s' > '%s' 2>/dev/null",
             apk, tar_path);
    if (command_ok(cmd)) {
        snprintf(cmd, sizeof(cmd), "tar -tf '%s' >/dev/null 2>&1", tar_path);
        if (command_ok(cmd))
            return 0;
        /* Old 3-stream APKs have zero blocks between members. */
        snprintf(cmd, sizeof(cmd),
                 "tar --ignore-zeros -tf '%s' >/dev/null 2>&1", tar_path);
        if (command_ok(cmd))
            return 0;
    }

    /* Fallback for toolchains where `gzip` is missing but tar has built-in
     * gzip support (e.g. busybox tar). This is the AvoryOS case from the
     * xeyes report: `tar -tzf` succeeds while `gzip -dc` fails. Materialize
     * the plain tar using only tar: unpack with tar's internal inflate,
     * then repack. Shell `>` redirection is avoided entirely here. */
    snprintf(cmd, sizeof(cmd), "tar -tzf '%s' >/dev/null 2>&1", apk);
    if (command_ok(cmd)) {
        snprintf(cmd, sizeof(cmd), "busybox gzip -dc '%s' > '%s' 2>/dev/null",
                 apk, tar_path);
        if (command_ok(cmd)) {
            snprintf(cmd, sizeof(cmd), "tar -tf '%s' >/dev/null 2>&1",
                     tar_path);
            if (command_ok(cmd))
                return 0;
        }
        {
            char tmpdir[PATH_MAX];
            snprintf(tmpdir, sizeof(tmpdir), "%s.fallback-%ld", tar_path,
                     (long)getpid());
            snprintf(cmd, sizeof(cmd),
                     "rm -rf '%s' && mkdir -p '%s' && "
                     "tar -xzf '%s' -C '%s' 2>/dev/null && "
                     "tar -cf '%s' -C '%s' . 2>/dev/null",
                     tmpdir, tmpdir, apk, tmpdir, tar_path, tmpdir);
            if (command_ok(cmd)) {
                snprintf(cmd, sizeof(cmd), "tar -tf '%s' >/dev/null 2>&1",
                         tar_path);
                if (command_ok(cmd)) {
                    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
                    command(cmd);
                    return 0;
                }
            }
            snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
            command(cmd);
        }
        fprintf(stderr,
                "[apm] APK is a valid gzip tar but no working gzip found\n");
    }

    unlink(tar_path);
    return -1;
}

static int path_is_safe_tar_name(const char *name)
{
    if (!name || !*name)
        return 0;

    while (*name == '/')
        name++;

    if (!*name)
        return 1;

    char part[PATH_MAX];
    size_t n = strlen(name);
    if (n >= sizeof(part))
        return 0;
    memcpy(part, name, n + 1);

    char *s = part;
    while (*s) {
        char *slash = strchr(s, '/');
        if (slash) *slash = '\0';
        if (strcmp(s, "..") == 0)
            return 0;
        if (slash) {
            s = slash + 1;
            while (*s == '/') s++;
        } else break;
    }
    return 1;
}

static int build_manifest(const char *tar_path, const char *manifest)
{
    char listing[PATH_MAX];
    snprintf(listing, sizeof(listing), "%s.list", tar_path);

    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "tar -tf '%s' > '%s' 2>/dev/null", tar_path,
             listing);
    if (!command_ok(cmd)) {
        /* Old 3-stream APKs need --ignore-zeros (see setup-arch.sh). */
        snprintf(cmd, sizeof(cmd),
                 "tar --ignore-zeros -tf '%s' > '%s' 2>/dev/null", tar_path,
                 listing);
        if (!command_ok(cmd)) {
            unlink(listing);
            return -1;
        }
    }

    FILE *in = fopen(listing, "r");
    FILE *out = fopen(manifest, "w");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        unlink(listing);
        return -1;
    }

    char line[PATH_MAX];
    while (fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || !path_is_safe_tar_name(line)) {
            fclose(in); fclose(out); unlink(listing); return -1;
        }
        if (strcmp(line, ".PKGINFO") == 0 ||
            strncmp(line, ".SIGN.", 6) == 0 ||
            strncmp(line, "./.SIGN.", 8) == 0 ||
            strncmp(line, ".INSTALL", 8) == 0 ||
            strncmp(line, ".pre", 4) == 0 ||
            strncmp(line, ".post", 5) == 0 ||
            strncmp(line, ".trigger", 8) == 0)
            continue;

        if (line[0] == '/')
            fprintf(out, "%s\n", line);
        else
            fprintf(out, "/%s\n", line);
    }

    fclose(in);
    fclose(out);
    unlink(listing);
    return 0;
}

static int install_tar(const char *tar_path, const char *root)
{
    char cmd[PATH_MAX * 3];

    /*
     * Important: --overwrite prevents O_EXCL-style failures when a package
     * replaces a file already present on the target OS.  We intentionally do
     * not restore Alpine's uid/gid/mode timestamps; those are host-OS policy.
     * --touch avoids utime/utimensat failures on minimal kernels/VFSes.
     * --ignore-zeros handles old 3-stream APK padding (setup-arch.sh).
     */
    snprintf(cmd, sizeof(cmd),
             "tar --ignore-zeros -xf '%s' -C '%s' --overwrite --no-same-owner "
             "--no-same-permissions --touch "
             "--exclude=.PKGINFO --exclude=.SIGN.* --exclude=.INSTALL "
             "--exclude=.pre-install --exclude=.post-install "
             "--exclude=.pre-deinstall --exclude=.post-deinstall "
             "--exclude=.pre-upgrade --exclude=.post-upgrade "
             "--exclude=.trigger 2>/dev/null",
             tar_path, root);
    if (command_ok(cmd))
        return 0;

    /* Busybox tar may lack --overwrite/--touch/--exclude/--ignore-zeros:
     * retry with a portable subset. Metadata strays stay in staging and
     * are never moved into / (build_manifest filters them). */
    snprintf(cmd, sizeof(cmd),
             "tar -xf '%s' -C '%s' --no-same-owner "
             "--no-same-permissions --touch 2>/dev/null",
             tar_path, root);
    if (command_ok(cmd))
        return 0;

    /* Last resort: plain extraction (tar_path is already plain tar). */
    snprintf(cmd, sizeof(cmd), "tar -xf '%s' -C '%s' 2>/dev/null",
             tar_path, root);
    return command_ok(cmd) ? 0 : -1;
}

static int write_metadata(const char *name, const Package *p,
                          const char *manifest)
{
    char dir[PATH_MAX], meta[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s", APM_INSTALLED, name);
    snprintf(meta, sizeof(meta), "%s/info", dir);

    if (mkdir_p(dir, 0755) < 0)
        return -1;

    FILE *f = fopen(meta, "w");
    if (!f)
        return -1;

    fprintf(f, "NAME=%s\nVERSION=%s\nARCH=%s\nREPO=%s/%s\n",
            p->name, p->version, p->arch, p->branch, p->repo);
    fprintf(f, "APK=%s-%s.apk\n", p->name, p->version);
    fprintf(f, "MANIFEST=%s/files\n", dir);
    fclose(f);

    char dst[PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/files", dir);
    int in = open(manifest, O_RDONLY);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) {
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        return -1;
    }

    unsigned char buf[8192];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(in); close(out); return -1;
        }
        if (n == 0) break;
        if (write_all_fd(out, buf, (size_t)n) < 0) {
            close(in); close(out); return -1;
        }
    }
    close(in);
    close(out);
    return 0;
}

static int dependency_name(const char *token, char *out, size_t out_sz)
{
    size_t n = 0;
    while (token[n] && token[n] != '<' && token[n] != '>' &&
           token[n] != '=' && token[n] != '~') n++;
    if (n == 0 || n >= out_sz) return -1;
    memcpy(out, token, n);
    out[n] = '\0';
    return 0;
}

static int dependency_cb(const Package *p, void *arg)
{
    const char *wanted = arg;
    return strcmp(p->name, wanted) == 0;
}

static int package_exists_in_indexes(const char *name)
{
    for (int i = 0; repos[i].branch; ++i)
        if (foreach_index(repos[i].branch, repos[i].name, dependency_cb, (void *)name))
            return 1;
    return 0;
}

static int install_one(const char *name, int depth);

static int install_dependencies(const Package *p, int depth)
{
    if (!p->deps[0]) return 0;
    if (depth > 32) {
        fprintf(stderr, "[apm] dependency depth exceeded while installing %s\n", p->name);
        return -1;
    }

    char deps[sizeof(p->deps)];
    snprintf(deps, sizeof(deps), "%s", p->deps);
    char *save = NULL;
    for (char *tok = strtok_r(deps, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (strncmp(tok, "so:", 3) == 0 || strncmp(tok, "cmd:", 4) == 0)
            continue;
        char dep[256];
        if (dependency_name(tok, dep, sizeof(dep)) < 0)
            continue;
        if (!package_exists_in_indexes(dep))
            continue;
        if (install_one(dep, depth + 1) != 0)
            return -1;
    }
    return 0;
}

static int install_one(const char *name, int depth)
{
    if (!install_path_safe(name)) {
        fprintf(stderr, "[apm] invalid package name\n");
        return 1;
    }

    if (mkdir_p(APM_INSTALLED, 0755) < 0 || mkdir_p(APM_TMP, 0755) < 0) {
        fprintf(stderr, "[apm] cannot create APM directories: %s\n", strerror(errno));
        return 1;
    }

    char installed_dir[PATH_MAX];
    snprintf(installed_dir, sizeof(installed_dir), "%s/%s", APM_INSTALLED, name);
    if (file_exists(installed_dir)) {
        printf("[apm] %s is already installed\n", name);
        return 0;
    }

    Package p;
    if (find_package(name, &p) < 0) {
        fprintf(stderr, "[apm] package '%s' not found; run 'apm update' first\n", name);
        return 1;
    }

    if (install_dependencies(&p, depth) < 0)
        return 1;

    char apk[PATH_MAX], tar_path[PATH_MAX], stage[PATH_MAX], manifest[PATH_MAX];
    snprintf(apk, sizeof(apk), "%s/%s-%s.apk", APM_TMP, p.name, p.version);
    snprintf(tar_path, sizeof(tar_path), "%s/%s-%s.tar", APM_TMP, p.name, p.version);
    snprintf(stage, sizeof(stage), "%s/stage-%s-%ld", APM_TMP, name, (long)getpid());
    snprintf(manifest, sizeof(manifest), "%s/%s-%s.manifest", APM_TMP, p.name, p.version);

    char url[PATH_MAX], cmd[PATH_MAX * 2];
    snprintf(url, sizeof(url), "%s/%s/%s/%s/%s-%s.apk",
             ALPINE_CDN, p.branch, p.repo, ALPINE_ARCH, p.name, p.version);

    printf("[apm] downloading %s\n", url);
    snprintf(cmd, sizeof(cmd), "curl -fL --progress-bar '%s' -o '%s'", url, apk);
    if (!command_ok(cmd)) {
        fprintf(stderr, "[apm] download failed\n");
        unlink(apk);
        return 1;
    }

    printf("[apm] unpacking %s\n", p.name);
    if (extract_data_stream(apk, tar_path) < 0) {
        fprintf(stderr, "[apm] could not locate a valid APK data tar stream\n");
        unlink(apk); unlink(tar_path);
        return 1;
    }

    if (mkdir_p(stage, 0755) < 0) {
        fprintf(stderr, "[apm] cannot create staging directory: %s\n", strerror(errno));
        unlink(apk); unlink(tar_path); return 1;
    }

    /* Validate every archive member before touching the target filesystem. */
    if (build_manifest(tar_path, manifest) < 0) {
        fprintf(stderr, "[apm] unsafe or unreadable tar archive\n");
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", stage);
        command(cmd);
        unlink(apk); unlink(tar_path); unlink(manifest);
        return 1;
    }

    printf("[apm] extracting to staging directory\n");
    if (install_tar(tar_path, stage) < 0) {
        fprintf(stderr, "[apm] extraction failed\n");
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", stage);
        command(cmd);
        unlink(apk); unlink(tar_path); unlink(manifest);
        return 1;
    }

    /* Build final directories before moving files into /. */
    FILE *mf = fopen(manifest, "r");
    if (!mf) {
        fprintf(stderr, "[apm] cannot read manifest\n");
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", stage); command(cmd);
        unlink(apk); unlink(tar_path); return 1;
    }

    char line[PATH_MAX];
    while (fgets(line, sizeof(line), mf)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;

        char source[PATH_MAX], target[PATH_MAX];
        const char *relative = line[0] == '/' ? line + 1 : line;
        snprintf(source, sizeof(source), "%s/%s", stage, relative);
        snprintf(target, sizeof(target), "/%s", relative);

        struct stat st;
        if (lstat(source, &st) < 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            /* Directories are merged, never replaced. */
            if (mkdir_p(target, 0755) < 0) {
                fprintf(stderr, "[apm] cannot create %s: %s\n", target, strerror(errno));
                fclose(mf);
                goto fail;
            }
            continue;
        }

        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", target);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            if (mkdir_p(parent, 0755) < 0) {
                fprintf(stderr, "[apm] cannot create parent %s\n", parent);
                fclose(mf);
                goto fail;
            }
        }

        /* Replace existing file/symlink. Never use rename() across filesystems. */
        if (unlink(target) < 0 && errno != ENOENT && !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "[apm] cannot replace %s: %s\n", target, strerror(errno));
            fclose(mf);
            goto fail;
        }

        if (rename(source, target) < 0) {
            /* rename can fail across filesystems (EXDEV); fall back per type. */
            if (S_ISREG(st.st_mode)) {
                int in = open(source, O_RDONLY);
                int out = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0755);
                if (in < 0 || out < 0) {
                    if (in >= 0) close(in);
                    if (out >= 0) close(out);
                    fprintf(stderr, "[apm] cannot install %s: %s\n", target, strerror(errno));
                    fclose(mf);
                    goto fail;
                }
                unsigned char copybuf[65536];
                for (;;) {
                    ssize_t n = read(in, copybuf, sizeof(copybuf));
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        close(in); close(out); fclose(mf); goto fail;
                    }
                    if (n == 0) break;
                    if (write_all_fd(out, copybuf, (size_t)n) < 0) {
                        close(in); close(out); fclose(mf); goto fail;
                    }
                }
                close(in); close(out); unlink(source);
            } else if (S_ISLNK(st.st_mode)) {
                /* Cross-filesystem symlink: recreate at target path. */
                char lnkbuf[PATH_MAX];
                ssize_t llen = readlink(source, lnkbuf, sizeof(lnkbuf) - 1);
                if (llen < 0) {
                    fprintf(stderr, "[apm] cannot read symlink %s: %s\n",
                            source, strerror(errno));
                    fclose(mf);
                    goto fail;
                }
                lnkbuf[llen] = '\0';
                unlink(target); /* remove any leftover */
                if (symlink(lnkbuf, target) < 0) {
                    fprintf(stderr, "[apm] cannot create symlink %s -> %s: %s\n",
                            target, lnkbuf, strerror(errno));
                    fclose(mf);
                    goto fail;
                }
                unlink(source);
            } else {
                fprintf(stderr, "[apm] cannot install %s: %s\n", target, strerror(errno));
                fclose(mf);
                goto fail;
            }
        }
    }
    fclose(mf);

    if (write_metadata(name, &p, manifest) < 0) {
        fprintf(stderr, "[apm] package installed but metadata could not be written\n");
        goto fail_cleanup;
    }

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", stage); command(cmd);
    unlink(apk); unlink(tar_path); unlink(manifest);
    printf("[apm] installed %s-%s\n", p.name, p.version);
    return 0;

fail:
    /* Do not attempt an unsafe rollback here; files already moved remain valid. */
    fprintf(stderr, "[apm] installation stopped; package may be partially installed\n");
fail_cleanup:
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", stage); command(cmd);
    unlink(apk); unlink(tar_path); unlink(manifest);
    return 1;
}

static int cmd_install(const char *name)
{
    return install_one(name, 0);
}

static int search_cb(const Package *p, void *arg)
{
    const char *needle = arg;
    if (strstr(p->name, needle) || strstr(p->desc, needle)) {
        printf("%-30s %-16s %s/%s\n", p->name, p->version, p->branch, p->repo);
    }
    return 0;
}

static int info_cb(const Package *p, void *arg)
{
    const char *name = arg;
    if (strcmp(p->name, name) != 0)
        return 0;
    printf("Name: %s\nVersion: %s\nArch: %s\nRepo: %s/%s\nDescription: %s\n",
           p->name, p->version, p->arch, p->branch, p->repo, p->desc);
    printf("Size: %s\nInstalled size: %s\n", p->size, p->isize);
    if (p->deps[0]) printf("Depends: %s\n", p->deps);
    if (p->url[0]) printf("URL: %s\n", p->url);
    return 1;
}

static int cmd_update(void)
{
    int failed = 0;
    for (int i = 0; repos[i].branch; ++i) {
        if (fetch_index(repos[i].branch, repos[i].name) < 0) {
            printf("[apm] WARNING: failed to update %s/%s\n",
                   repos[i].branch, repos[i].name);
            failed = 1;
        }
    }
    if (failed)
        printf("[apm] update completed with errors\n");
    else
        printf("[apm] all indexes updated successfully\n");
    return failed;
}

static int cmd_search(const char *needle)
{
    for (int i = 0; repos[i].branch; ++i)
        foreach_index(repos[i].branch, repos[i].name, search_cb, (void *)needle);
    return 0;
}

static int cmd_info(const char *name)
{
    for (int i = 0; repos[i].branch; ++i)
        if (foreach_index(repos[i].branch, repos[i].name, info_cb, (void *)name))
            return 0;
    fprintf(stderr, "[apm] package '%s' not found\n", name);
    return 1;
}

static int cmd_list(void)
{
    if (!file_exists(APM_INSTALLED)) {
        printf("[apm] no packages installed\n");
        return 0;
    }

    char cmd[PATH_MAX];
    snprintf(cmd, sizeof(cmd), "find '%s' -mindepth 1 -maxdepth 1 -type d -printf '%%f\\n' 2>/dev/null", APM_INSTALLED);
    command(cmd);
    return 0;
}

static int cmd_remove(const char *name)
{
    if (!install_path_safe(name))
        return 1;

    char dir[PATH_MAX], manifest[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s", APM_INSTALLED, name);
    snprintf(manifest, sizeof(manifest), "%s/files", dir);

    FILE *f = fopen(manifest, "r");
    if (!f) {
        fprintf(stderr, "[apm] package '%s' is not installed\n", name);
        return 1;
    }

    /* Remove deepest paths first. */
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "sort -r '%s' > '%s.sorted'", manifest, manifest);
    if (!command_ok(cmd)) {
        fclose(f);
        return 1;
    }
    fclose(f);

    char sorted[PATH_MAX];
    snprintf(sorted, sizeof(sorted), "%s.sorted", manifest);
    f = fopen(sorted, "r");
    if (!f) return 1;

    char line[PATH_MAX];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || !path_is_safe_tar_name(line)) continue;
        if (strcmp(line, "/") == 0) continue;
        unlink(line);
    }
    fclose(f);
    unlink(sorted);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    command(cmd);
    printf("[apm] removed %s\n", name);
    return 0;
}

static void usage(void)
{
    puts("APM - Alpine package manager for AscentOS/Linux");
    puts("Usage: apm <command> [package]");
    puts("");
    puts("  update             update Alpine indexes");
    puts("  search <name>      search packages");
    puts("  info <name>        show package information");
    puts("  install <name>     download and install package");
    puts("  remove <name>      remove installed package files");
    puts("  list               list installed packages");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "update") == 0)
        return cmd_update();
    if (strcmp(argv[1], "search") == 0 && argc >= 3)
        return cmd_search(argv[2]);
    if (strcmp(argv[1], "info") == 0 && argc >= 3)
        return cmd_info(argv[2]);
    if (strcmp(argv[1], "install") == 0 && argc >= 3)
        return cmd_install(argv[2]);
    if (strcmp(argv[1], "remove") == 0 && argc >= 3)
        return cmd_remove(argv[2]);
    if (strcmp(argv[1], "list") == 0)
        return cmd_list();

    usage();
    return 1;
}