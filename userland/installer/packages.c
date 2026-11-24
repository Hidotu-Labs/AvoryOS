#include "packages.h"
#include "install.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PKG_DB "/var/lib/pacman/local"

/* Always installed: init/userland the live image boots with, networking,
 * audio, the package manager, and a C toolchain. */
static const char *const base_pkgs[] = {
    "filesystem", "glibc", "busybox", "bash", "coreutils", "util-linux",
    "shadow", "sudo", "pam", "acl", "sed", "grep", "gawk", "tar", "gzip",
    "xz", "bzip2", "zstd", "lz4", "unzip", "zip", "e2fsprogs", "parted",
    "file", "nano", "htop", "fastfetch", "pciutils", "curl", "dosfstools", "mtools", "limine",
    "ca-certificates", "openssl", "tzdata", "dbus", "seatd", "polkit",
    "systemd", "accountsservice", "networkmanager", "wpa_supplicant",
    "pacman", "archlinux-keyring", "libarchive", "python",
    "alsa-lib", "alsa-plugins", "pipewire", "pipewire-pulse",
    "pipewire-alsa", "wireplumber",
    "gcc", "binutils", "make", "pkgconf",
    NULL
};

/* Shared by every graphical desktop (Xorg + SDDM display manager). */
static const char *const xorg_pkgs[] = {
    "xorg-server", "xorg-xinit", "xorg-xauth", "xorg-xrandr",
    "xorg-setxkbmap", "xorg-xkbcomp", "xkeyboard-config",
    "xf86-input-libinput", "xf86-input-evdev", "mesa", "mesa-utils", "vulkan-radeon", "libdrm",
    "xorg-fonts-misc", "ttf-dejavu", "noto-fonts", "xdg-utils",
    "xdg-user-dirs", "sddm",
    NULL
};

static const char *const plasma_pkgs[] = {
    "plasma-desktop", "plasma-workspace", "kwin", "sddm", "konsole",
    "dolphin", "systemsettings", "plasma-nm", "plasma-pa", "powerdevil",
    "kscreen", "breeze", "kde-gtk-config", "xdg-desktop-portal-kde",
    "polkit-kde-agent", "kinfocenter", "ark",
    NULL
};

static const char *const xfce_pkgs[] = {
    "xfce4-session", "xfwm4", "xfdesktop", "xfce4-panel", "xfce4-settings",
    "xfce4-terminal", "thunar", "xfce4-appfinder", "xfce4-notifyd",
    "xfce4-power-manager", "mousepad",
    NULL
};

static const char *const icewm_pkgs[] = {"icewm", "alacritty", "xterm", NULL};

/* Top-level trees scanned for files no package owns. */
static const char *const walk_roots[] = {
    "bin", "sbin", "lib", "lib64", "usr", "etc", "opt", "var", "root", NULL
};

/* Caches, logs, the host-side Axon state and the build-time duplicate of the
 * glibc headers stay on the live image. The pacman database is copied
 * per selected package instead. */
static const char *const walk_skip[] = {
    "var/cache", "var/log", "var/tmp", "var/lib/pacman", "var/lib/axon",
    "opt/glibc", NULL
};

/* ── string hash map ───────────────────────────────────────────────────── */

typedef struct {
    char *key;
    uintptr_t val;
} hent_t;

typedef struct {
    hent_t *e;
    size_t cap;
    size_t n;
} hmap_t;

static uint64_t hash_str(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static hent_t *hmap_slot(hent_t *e, size_t cap, const char *key) {
    size_t i = (size_t)hash_str(key) & (cap - 1);
    while (e[i].key && strcmp(e[i].key, key) != 0) i = (i + 1) & (cap - 1);
    return &e[i];
}

static hent_t *hmap_find(const hmap_t *m, const char *key) {
    if (!m->cap) return NULL;
    hent_t *s = hmap_slot(m->e, m->cap, key);
    return s->key ? s : NULL;
}

/* Returns the entry for key, inserting a copy of key with val 0 if absent. */
static hent_t *hmap_upsert(hmap_t *m, const char *key) {
    if ((m->n + 1) * 2 > m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 1024;
        hent_t *ne = calloc(ncap, sizeof(*ne));
        if (!ne) return NULL;
        for (size_t i = 0; i < m->cap; i++) {
            if (m->e[i].key) *hmap_slot(ne, ncap, m->e[i].key) = m->e[i];
        }
        free(m->e);
        m->e = ne;
        m->cap = ncap;
    }
    hent_t *s = hmap_slot(m->e, m->cap, key);
    if (!s->key) {
        s->key = strdup(key);
        if (!s->key) return NULL;
        s->val = 0;
        m->n++;
    }
    return s;
}

static void hmap_free(hmap_t *m, bool free_vals) {
    for (size_t i = 0; i < m->cap; i++) {
        free(m->e[i].key);
        if (free_vals) free((void *)m->e[i].val);
    }
    free(m->e);
    memset(m, 0, sizeof(*m));
}

/* ── package database ──────────────────────────────────────────────────── */

typedef struct {
    char *name;
    char *dir;
    char **deps;
    int ndeps;
    uint64_t size;
    bool selected;
} pkg_t;

#define OWNED     1u
#define OWNED_SEL 2u

struct pkgsel {
    pkg_t *pkgs;
    int npkgs;
    int nselected;
    uint64_t bytes;
    hmap_t owned;   /* path -> OWNED | OWNED_SEL */
    char **paths;   /* selected packages' paths, keys of owned */
    size_t npaths;
};

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t n;
    while (buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 2) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    if (buf) buf[len] = '\0';
    return buf;
}

/* Drop version constraints: "libfoo.so=8-64" -> "libfoo.so". */
static void strip_constraint(char *s) {
    s[strcspn(s, "<>=")] = '\0';
}

static bool parse_desc(pkg_t *p, const char *dir) {
    char path[512];
    snprintf(path, sizeof(path), PKG_DB "/%s/desc", dir);
    char *buf = read_file(path);
    if (!buf) return false;

    p->dir = strdup(dir);
    char *section = "";
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(line);
        if (l > 2 && line[0] == '%' && line[l - 1] == '%') {
            section = line;
            continue;
        }
        if (strcmp(section, "%NAME%") == 0 && !p->name) {
            p->name = strdup(line);
        } else if (strcmp(section, "%SIZE%") == 0) {
            p->size = strtoull(line, NULL, 10);
        } else if (strcmp(section, "%DEPENDS%") == 0) {
            char **nd = realloc(p->deps, sizeof(char *) * (size_t)(p->ndeps + 1));
            if (!nd) break;
            p->deps = nd;
            p->deps[p->ndeps] = strdup(line);
            strip_constraint(p->deps[p->ndeps]);
            p->ndeps++;
        }
    }
    bool ok = p->name != NULL;
    free(buf);
    return ok;
}

static void pkg_free(pkg_t *p) {
    free(p->name);
    free(p->dir);
    for (int i = 0; i < p->ndeps; i++) free(p->deps[i]);
    free(p->deps);
}

void pkgsel_free(pkgsel_t *sel) {
    if (!sel) return;
    for (int i = 0; i < sel->npkgs; i++) pkg_free(&sel->pkgs[i]);
    free(sel->pkgs);
    hmap_free(&sel->owned, false);
    free(sel->paths);
    free(sel);
}

static int lookup_pkg(const hmap_t *names, const char *name) {
    hent_t *e = hmap_find(names, name);
    return e ? (int)e->val - 1 : -1;
}

static void select_roots(pkgsel_t *sel, const hmap_t *names, const char *const *roots,
                         int *stack, int *sp) {
    char msg[160];
    for (int i = 0; roots[i]; i++) {
        int idx = lookup_pkg(names, roots[i]);
        if (idx < 0) {
            snprintf(msg, sizeof(msg), "[pkgsel] %s is not in the live image; skipped", roots[i]);
            install_log(msg);
            continue;
        }
        stack[(*sp)++] = idx;
    }
    while (*sp > 0) {
        pkg_t *p = &sel->pkgs[stack[--(*sp)]];
        if (p->selected) continue;
        p->selected = true;
        sel->nselected++;
        sel->bytes += p->size;
        for (int d = 0; d < p->ndeps; d++) {
            int idx = lookup_pkg(names, p->deps[d]);
            if (idx < 0) {
                snprintf(msg, sizeof(msg), "[pkgsel] %s: unresolved dependency %s", p->name, p->deps[d]);
                install_log(msg);
                continue;
            }
            if (!sel->pkgs[idx].selected) stack[(*sp)++] = idx;
        }
    }
}

static bool load_files(pkgsel_t *sel, const pkg_t *p) {
    char path[512];
    snprintf(path, sizeof(path), PKG_DB "/%s/files", p->dir);
    char *buf = read_file(path);
    if (!buf) return true; /* metapackages may have none */

    bool in_files = false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '%') {
            in_files = strcmp(line, "%FILES%") == 0;
            continue;
        }
        if (!in_files) continue;
        size_t l = strlen(line);
        while (l > 1 && line[l - 1] == '/') line[--l] = '\0';

        hent_t *e = hmap_upsert(&sel->owned, line);
        if (!e) {
            free(buf);
            return false;
        }
        e->val |= OWNED;
        if (p->selected && !(e->val & OWNED_SEL)) {
            e->val |= OWNED_SEL;
            if ((sel->npaths & 4095) == 0) {
                char **np = realloc(sel->paths, sizeof(char *) * (sel->npaths + 4096));
                if (!np) {
                    free(buf);
                    return false;
                }
                sel->paths = np;
            }
            sel->paths[sel->npaths++] = e->key;
        }
    }
    free(buf);
    return true;
}

pkgsel_t *pkgsel_load(const installer_cfg_t *cfg, char *err_buf, size_t err_sz) {
    pkgsel_t *sel = calloc(1, sizeof(*sel));
    if (!sel) {
        snprintf(err_buf, err_sz, "Out of memory");
        return NULL;
    }

    DIR *dir = opendir(PKG_DB);
    if (!dir) {
        snprintf(err_buf, err_sz, "Cannot read package database %s", PKG_DB);
        free(sel);
        return NULL;
    }

    hmap_t names = {0}; /* name or provide -> index + 1 */
    struct dirent *ent;
    int cap = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (sel->npkgs == cap) {
            cap = cap ? cap * 2 : 256;
            pkg_t *np = realloc(sel->pkgs, sizeof(pkg_t) * (size_t)cap);
            if (!np) break;
            sel->pkgs = np;
        }
        pkg_t *p = &sel->pkgs[sel->npkgs];
        memset(p, 0, sizeof(*p));
        if (!parse_desc(p, ent->d_name)) {
            pkg_free(p);
            continue;
        }
        sel->npkgs++;
    }
    closedir(dir);

    for (int i = 0; i < sel->npkgs; i++) {
        hent_t *e = hmap_upsert(&names, sel->pkgs[i].name);
        if (e) e->val = (uintptr_t)i + 1;
    }
    /* Provides go in after every real name so packages win over them. */
    for (int i = 0; i < sel->npkgs; i++) {
        char path[512];
        snprintf(path, sizeof(path), PKG_DB "/%s/desc", sel->pkgs[i].dir);
        char *buf = read_file(path);
        if (!buf) continue;
        char *section = "";
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            size_t l = strlen(line);
            if (l > 2 && line[0] == '%' && line[l - 1] == '%') {
                section = line;
                continue;
            }
            if (strcmp(section, "%PROVIDES%") != 0) continue;
            strip_constraint(line);
            hent_t *e = hmap_upsert(&names, line);
            if (e && !e->val) e->val = (uintptr_t)i + 1;
        }
        free(buf);
    }

    /* Each package is pushed once per dependent at most, plus the roots. */
    size_t stack_len = 256;
    for (int i = 0; i < sel->npkgs; i++) stack_len += (size_t)sel->pkgs[i].ndeps;
    int *stack = malloc(sizeof(int) * stack_len);
    if (!stack || sel->npkgs == 0) {
        snprintf(err_buf, err_sz, sel->npkgs ? "Out of memory" : "Package database %s is empty", PKG_DB);
        free(stack);
        hmap_free(&names, false);
        pkgsel_free(sel);
        return NULL;
    }
    int sp = 0;
    select_roots(sel, &names, base_pkgs, stack, &sp);
    if (cfg->desktop != DE_CONSOLE) select_roots(sel, &names, xorg_pkgs, stack, &sp);
    if (cfg->desktop == DE_PLASMA) select_roots(sel, &names, plasma_pkgs, stack, &sp);
    else if (cfg->desktop == DE_XFCE4) select_roots(sel, &names, xfce_pkgs, stack, &sp);
    else if (cfg->desktop == DE_ICEWM) select_roots(sel, &names, icewm_pkgs, stack, &sp);
    free(stack);
    hmap_free(&names, false);

    for (int i = 0; i < sel->npkgs; i++) {
        if (!load_files(sel, &sel->pkgs[i])) {
            snprintf(err_buf, err_sz, "Out of memory reading package file lists");
            pkgsel_free(sel);
            return NULL;
        }
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "[pkgsel] %d of %d packages selected, %llu MiB, %zu paths",
             sel->nselected, sel->npkgs, (unsigned long long)(sel->bytes >> 20), sel->npaths);
    install_log(msg);
    return sel;
}

int pkgsel_count(const pkgsel_t *sel) {
    return sel->nselected;
}

uint64_t pkgsel_installed_bytes(const pkgsel_t *sel) {
    return sel->bytes;
}

/* ── copying ───────────────────────────────────────────────────────────── */

typedef struct {
    hmap_t links; /* "dev:ino" -> malloc'd target path, for hard links */
    dev_t root_dev;
    char err[256];
} copy_ctx_t;

static void make_parents(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
}

static int copy_data(const char *src, const char *dst, const struct stat *st) {
    static char buf[1 << 16];
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0 && errno == ENOENT) {
        make_parents(dst);
        out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    }
    if (out < 0) {
        close(in);
        return -1;
    }
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                int saved = errno;
                close(in);
                close(out);
                errno = saved ? saved : ENOSPC;
                return -1;
            }
            off += w;
        }
    }
    int rc = n < 0 ? -1 : 0;
    if (fchown(out, st->st_uid, st->st_gid) != 0) { /* best effort */ }
    fchmod(out, st->st_mode & 07777);
    struct timespec ts[2] = {st->st_atim, st->st_mtim};
    futimens(out, ts);
    close(in);
    if (close(out) != 0) rc = -1;
    return rc;
}

/* Recreate one live-system entry at TARGET_MOUNT. Missing sources are not an
 * error: the image may have removed files a package lists. */
static int copy_entry(copy_ctx_t *cc, const char *rel, const struct stat *st) {
    char src[4096], dst[4096];
    snprintf(src, sizeof(src), "/%s", rel);
    snprintf(dst, sizeof(dst), TARGET_MOUNT "/%s", rel);

    if (S_ISDIR(st->st_mode)) {
        if (mkdir(dst, st->st_mode & 07777) != 0) {
            if (errno == ENOENT) {
                make_parents(dst);
                mkdir(dst, st->st_mode & 07777);
            } else if (errno != EEXIST) {
                return 0;
            }
        }
        if (lchown(dst, st->st_uid, st->st_gid) != 0) { /* best effort */ }
        chmod(dst, st->st_mode & 07777);
        return 0;
    }

    struct stat old;
    if (lstat(dst, &old) == 0 && !S_ISDIR(old.st_mode)) unlink(dst);

    if (S_ISLNK(st->st_mode)) {
        char target[4096];
        ssize_t n = readlink(src, target, sizeof(target) - 1);
        if (n < 0) {
            char msg[4200];
            snprintf(msg, sizeof(msg), "[pkgsel] skipped symlink /%s: %s", rel, strerror(errno));
            install_log(msg);
            return 0;
        }
        target[n] = '\0';
        if (symlink(target, dst) != 0 && errno == ENOENT) {
            make_parents(dst);
            symlink(target, dst);
        }
        if (lchown(dst, st->st_uid, st->st_gid) != 0) { /* best effort */ }
        return 0;
    }

    if (S_ISREG(st->st_mode)) {
        char key[48];
        hent_t *link_ent = NULL;
        if (st->st_nlink > 1) {
            snprintf(key, sizeof(key), "%lu:%lu", (unsigned long)st->st_dev, (unsigned long)st->st_ino);
            link_ent = hmap_upsert(&cc->links, key);
            if (link_ent && link_ent->val && link((const char *)link_ent->val, dst) == 0) return 0;
        }
        if (copy_data(src, dst, st) != 0) {
            if (errno == ENOENT) return 0;
            snprintf(cc->err, sizeof(cc->err), "Copying /%s failed: %s", rel, strerror(errno));
            return -1;
        }
        if (link_ent && !link_ent->val) link_ent->val = (uintptr_t)strdup(dst);
        return 0;
    }

    if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode) || S_ISFIFO(st->st_mode)) {
        mknod(dst, st->st_mode, st->st_rdev);
        if (lchown(dst, st->st_uid, st->st_gid) != 0) { /* best effort */ }
    }
    return 0;
}

static int copy_tree(copy_ctx_t *cc, const char *rel) {
    char src[4096];
    snprintf(src, sizeof(src), "/%s", rel);
    struct stat st;
    if (lstat(src, &st) != 0) return 0;
    if (copy_entry(cc, rel, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return 0;

    DIR *d = opendir(src);
    if (!d) return 0;
    struct dirent *ent;
    int rc = 0;
    while (rc == 0 && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", rel, ent->d_name);
        rc = copy_tree(cc, child);
    }
    closedir(d);
    return rc;
}

static bool skip_walk(const char *rel) {
    for (int i = 0; walk_skip[i]; i++) {
        if (strcmp(rel, walk_skip[i]) == 0) return true;
    }
    return false;
}

/* Copy entries no package owns. Owned directories are only entered when a
 * selected package owns them, so leftovers of unselected packages stay. */
static int copy_unowned(copy_ctx_t *cc, const pkgsel_t *sel, const char *rel) {
    if (skip_walk(rel)) return 0;
    char src[4096];
    snprintf(src, sizeof(src), "/%s", rel);
    struct stat st;
    /* Only directories can be mount points; checking st_dev on every entry
     * also dropped symlinks wherever the kernel reports them with st_dev 0. */
    if (lstat(src, &st) != 0 || (S_ISDIR(st.st_mode) && st.st_dev != cc->root_dev)) return 0;

    hent_t *e = hmap_find(&sel->owned, rel);
    if (e) {
        if (!S_ISDIR(st.st_mode) || !(e->val & OWNED_SEL)) return 0;
    } else if (copy_entry(cc, rel, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) return 0;

    DIR *d = opendir(src);
    if (!d) return 0;
    struct dirent *ent;
    int rc = 0;
    while (rc == 0 && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", rel, ent->d_name);
        rc = copy_unowned(cc, sel, child);
    }
    closedir(d);
    return rc;
}

int pkgsel_copy(const pkgsel_t *sel, install_progress_fn cb, void *ctx,
                int pct_lo, int pct_hi, char *err_buf, size_t err_sz) {
    copy_ctx_t cc = {0};
    struct stat root;
    if (stat("/", &root) == 0) cc.root_dev = root.st_dev;

    int span = pct_hi - pct_lo;
    int pkg_hi = pct_lo + span * 8 / 10;
    char stage[96];
    snprintf(stage, sizeof(stage), "Copying %d packages...", sel->nselected);

    int rc = 0;
    for (size_t i = 0; rc == 0 && i < sel->npaths; i++) {
        const char *rel = sel->paths[i];
        char src[4096];
        snprintf(src, sizeof(src), "/%s", rel);
        struct stat st;
        if (lstat(src, &st) != 0) continue;
        rc = copy_entry(&cc, rel, &st);
        if (cb && (i & 1023) == 0) {
            cb(pct_lo + (int)((uint64_t)(pkg_hi - pct_lo) * i / sel->npaths), stage, rel, ctx);
        }
    }

    if (rc == 0) {
        if (cb) cb(pkg_hi, "Copying package database...", NULL, ctx);
        rc = copy_tree(&cc, "var/lib/pacman/local/ALPM_DB_VERSION");
        for (int i = 0; rc == 0 && i < sel->npkgs; i++) {
            if (!sel->pkgs[i].selected) continue;
            char rel[512];
            snprintf(rel, sizeof(rel), "var/lib/pacman/local/%s", sel->pkgs[i].dir);
            rc = copy_tree(&cc, rel);
        }
        if (rc == 0) rc = copy_tree(&cc, "var/lib/pacman/sync");
        if (rc == 0) rc = copy_tree(&cc, "var/lib/axon/sync");
    }

    for (int i = 0; rc == 0 && walk_roots[i]; i++) {
        if (cb) cb(pkg_hi + (span - (pkg_hi - pct_lo)) * i / 10, "Copying AvoryOS system files...", walk_roots[i], ctx);
        rc = copy_unowned(&cc, sel, walk_roots[i]);
    }

    if (rc != 0) {
        snprintf(err_buf, err_sz, "%s", cc.err[0] ? cc.err : "Copy failed");
        install_log(err_buf);
    }
    hmap_free(&cc.links, true);
    return rc;
}
