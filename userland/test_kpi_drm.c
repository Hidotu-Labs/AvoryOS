// AvoryOS Phase 3 LinuxKPI DRM userland test (the userland half of C4/C5).
//
// Runs against the imported upstream DRM core's vkms canary:
//
//   1. renderD128 sanity: open + DRM_IOCTL_VERSION + DRM_IOCTL_GET_CAP.
//   2. card1 GEM dumb buffer: CREATE_DUMB -> MAP_DUMB -> mmap -> write/read
//      pattern -> second mapping aliases the same pages -> munmap -> GEM_CLOSE.
//      This is the part the kernel suite cannot do: kpi_file_mmap() parks the
//      Linux VMA for the sys_mmap() path, so only userland can fault the pages.
//   3. modetest-lite: atomic client cap, connector/CRTC/plane property
//      discovery, ADDFB2 + MODE_CREATEPROPBLOB, atomic enable with
//      PAGE_FLIP_EVENT, poll()+read() of the flip event, WAIT_VBLANK, then an
//      atomic disable.
//
// Build: userland/test_kpi_drm.elf, installed as /bin/test_kpi_drm.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#define CARD_PATH "/dev/dri/card1"
#define RENDER_PATH "/dev/dri/renderD128"

#define DUMB_W 128
#define DUMB_H 128

static int failures;

static void pass(const char *what) { printf("  [PASS] %s\n", what); }

static void fail(const char *what, long detail) {
    failures++;
    printf("  [FAIL] %s", what);
    if (detail)
        printf(" (errno=%ld %s)", detail, strerror((int)detail));
    printf("\n");
}

/* ── render node sanity ─────────────────────────────────────────────────── */

static void test_render_node(void) {
    struct drm_version ver;
    struct drm_get_cap cap;
    char name[64];
    int fd;

    printf("\n=== renderD128 sanity ===\n");
    fd = open(RENDER_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("  [SKIP] %s not present (%s)\n", RENDER_PATH, strerror(errno));
        return;
    }

    memset(&ver, 0, sizeof(ver));
    memset(name, 0, sizeof(name));
    ver.name = name;
    ver.name_len = sizeof(name) - 1;
    if (ioctl(fd, DRM_IOCTL_VERSION, &ver) == 0 && ver.name_len)
        printf("  [PASS] renderD128 VERSION %d.%d.%d name=%s\n",
               ver.version_major, ver.version_minor, ver.version_patchlevel,
               name);
    else
        fail("renderD128 DRM_IOCTL_VERSION", errno);

    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CAP_TIMESTAMP_MONOTONIC;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1)
        pass("renderD128 GET_CAP(TIMESTAMP_MONOTONIC)");
    else
        fail("renderD128 GET_CAP", errno);

    close(fd);
}

/* ── GEM dumb buffer mmap/write ─────────────────────────────────────────── */

static void test_gem_mmap(int fd) {
    struct drm_mode_create_dumb cd;
    struct drm_mode_map_dumb md;
    struct drm_gem_close gc;
    unsigned char *p1, *p2;
    size_t i, j;
    int bad = 0;

    printf("\n=== card1 GEM dumb buffer mmap/write ===\n");

    memset(&cd, 0, sizeof(cd));
    cd.width = DUMB_W;
    cd.height = DUMB_H;
    cd.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) || !cd.handle) {
        fail("CREATE_DUMB", errno);
        return;
    }
    printf("  [PASS] CREATE_DUMB handle=%u pitch=%u size=%llu\n", cd.handle,
           cd.pitch, (unsigned long long)cd.size);

    if (cd.pitch < DUMB_W * 4 || cd.size < (uint64_t)cd.pitch * DUMB_H) {
        fail("CREATE_DUMB geometry", 0);
        goto out_close;
    }

    memset(&md, 0, sizeof(md));
    md.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) || !md.offset) {
        fail("MAP_DUMB", errno);
        goto out_close;
    }
    pass("MAP_DUMB returned an mmap offset");

    p1 = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p1 == MAP_FAILED) {
        fail("mmap(dumb handle)", errno);
        goto out_close;
    }
    pass("mmap(dumb handle)");

    /* Per-page pattern; touches several pages of the 128x128 buffer. */
    for (i = 0; i < cd.size / 4096; i++)
        for (j = 0; j < 4096; j++)
            p1[i * 4096 + j] = (unsigned char)(i * 37 + j);
    for (i = 0; i < cd.size / 4096 && !bad; i++)
        for (j = 0; j < 4096; j++)
            if (p1[i * 4096 + j] != (unsigned char)(i * 37 + j)) {
                bad = 1;
                break;
            }
    if (bad)
        fail("write/read back through the mapping", 0);
    else
        pass("write/read back through the mapping");

    /* A second mapping of the same BO must alias the same pages. */
    p2 = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p2 == MAP_FAILED) {
        fail("second mmap(dumb handle)", errno);
        munmap(p1, cd.size);
        goto out_close;
    }
    if (memcmp(p1, p2, cd.size) == 0) {
        pass("second mapping aliases the same pages");
    } else {
        fail("second mapping aliases the same pages", 0);
    }
    p2[4096 + 17] = 0x5A;
    if (p1[4096 + 17] == 0x5A)
        pass("writes through one mapping are visible in the other");
    else
        fail("shared writes between mappings", 0);

    if (munmap(p2, cd.size) == 0)
        pass("munmap(second mapping)");
    else
        fail("munmap(second mapping)", errno);
    if (munmap(p1, cd.size) == 0)
        pass("munmap(dumb handle)");
    else
        fail("munmap(dumb handle)", errno);

out_close:
    memset(&gc, 0, sizeof(gc));
    gc.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc) == 0)
        pass("GEM_CLOSE");
    else
        fail("GEM_CLOSE", errno);
}

/* ── modetest-lite: atomic modeset through property discovery ───────────── */

struct prop_set {
    uint32_t ids[64];
    uint64_t vals[64];
    uint32_t count;
};

struct atomic_req {
    uint32_t n_objs;
    uint32_t n_props;
    uint32_t objs[4];
    uint32_t counts[4];
    uint32_t props[32];
    uint64_t values[32];
};

static void req_add(struct atomic_req *r, uint32_t obj, const uint32_t *props,
                    const uint64_t *values, uint32_t n) {
    r->objs[r->n_objs] = obj;
    r->counts[r->n_objs] = n;
    r->n_objs++;
    for (uint32_t i = 0; i < n; i++) {
        r->props[r->n_props] = props[i];
        r->values[r->n_props] = values[i];
        r->n_props++;
    }
}

/* Discover one object's property IDs and current values. */
static int obj_props(int fd, uint32_t obj_id, uint32_t obj_type,
                     struct prop_set *out) {
    struct drm_mode_obj_get_properties p;
    uint32_t total;

    memset(&p, 0, sizeof(p));
    p.obj_id = obj_id;
    p.obj_type = obj_type;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p) || !p.count_props)
        return -1;
    total = p.count_props;
    if (total > 64)
        total = 64;

    memset(&p, 0, sizeof(p));
    p.obj_id = obj_id;
    p.obj_type = obj_type;
    p.count_props = total;
    p.props_ptr = (uint64_t)(uintptr_t)out->ids;
    p.prop_values_ptr = (uint64_t)(uintptr_t)out->vals;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p))
        return -1;
    out->count = p.count_props > 64 ? 64 : p.count_props;
    return 0;
}

static int prop_find(int fd, const struct prop_set *set, const char *name,
                     uint32_t *id_out, uint64_t *val_out) {
    struct drm_mode_get_property p;

    for (uint32_t i = 0; i < set->count; i++) {
        memset(&p, 0, sizeof(p));
        p.prop_id = set->ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p))
            return -1;
        if (strncmp(p.name, name, sizeof(p.name)) == 0) {
            if (id_out)
                *id_out = set->ids[i];
            if (val_out)
                *val_out = set->vals[i];
            return 0;
        }
    }
    return -1;
}

static int atomic_commit(int fd, struct atomic_req *r, uint32_t flags) {
    struct drm_mode_atomic at;

    memset(&at, 0, sizeof(at));
    at.flags = flags;
    at.count_objs = r->n_objs;
    at.objs_ptr = (uint64_t)(uintptr_t)r->objs;
    at.count_props_ptr = (uint64_t)(uintptr_t)r->counts;
    at.props_ptr = (uint64_t)(uintptr_t)r->props;
    at.prop_values_ptr = (uint64_t)(uintptr_t)r->values;
    return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at);
}

static void test_modeset(int fd) {
    struct drm_set_client_cap cap;
    struct drm_mode_card_res res;
    struct drm_mode_get_plane_res pres;
    struct drm_mode_get_connector con;
    struct drm_mode_modeinfo *modes = NULL;
    uint32_t mode_cap;
    struct prop_set conn_props, crtc_props, plane_props;
    struct drm_mode_create_dumb cd;
    struct drm_mode_fb_cmd2 fb;
    struct drm_mode_create_blob blob;
    struct drm_mode_destroy_blob dblob;
    struct drm_mode_destroy_dumb ddumb;
    struct atomic_req req;
    union drm_wait_vblank vbl;
    struct drm_event_vblank ev;
    struct pollfd pfd;
    uint32_t crtc_id = 0, conn_id = 0, plane_id = 0;
    uint32_t foo[16] = {0};
    uint32_t conn_crtc_id, crtc_mode_id, crtc_active, plane_fb_id, plane_crtc_id;
    uint32_t p_src_x, p_src_y, p_src_w, p_src_h, p_crtc_x, p_crtc_y, p_crtc_w,
        p_crtc_h;
    uint64_t type_val;
    uint32_t props[16];
    uint64_t values[16];
    uint32_t n;
    int ret;

    printf("\n=== card1 atomic modetest-lite (enable/event/vblank/disable) ===\n");

    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CLIENT_CAP_ATOMIC;
    cap.value = 1;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap)) {
        fail("SET_CLIENT_CAP(ATOMIC)", errno);
        return;
    }

    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs ||
        !res.count_connectors) {
        fail("GETRESOURCES", errno);
        return;
    }
    memset(&res, 0, sizeof(res));
    res.crtc_id_ptr = (uint64_t)(uintptr_t)foo;
    res.connector_id_ptr = (uint64_t)(uintptr_t)(foo + 4);
    res.count_crtcs = res.count_connectors = 4;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        fail("GETRESOURCES(ids)", errno);
        return;
    }
    crtc_id = foo[0];
    conn_id = foo[4];

    memset(&pres, 0, sizeof(pres));
    pres.plane_id_ptr = (uint64_t)(uintptr_t)foo;
    pres.count_planes = 4;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) || !pres.count_planes) {
        fail("GETPLANERESOURCES", errno);
        return;
    }

    memset(&con, 0, sizeof(con));
    con.connector_id = conn_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &con) || !con.count_modes) {
        fail("GETCONNECTOR(counts)", errno);
        return;
    }
    mode_cap = con.count_modes;
    modes = calloc(mode_cap, sizeof(*modes));
    if (!modes) {
        fail("calloc modes", 0);
        return;
    }
    memset(&con, 0, sizeof(con));
    con.connector_id = conn_id;
    con.count_modes = mode_cap;
    con.modes_ptr = (uint64_t)(uintptr_t)modes;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &con)) {
        fail("GETCONNECTOR(modes)", errno);
        free(modes);
        return;
    }
    printf("  [PASS] connector=%u crtc=%u modes=%u (%ux%u)\n", conn_id, crtc_id,
           con.count_modes, modes[0].hdisplay, modes[0].vdisplay);

    if (obj_props(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, &conn_props) ||
        obj_props(fd, crtc_id, DRM_MODE_OBJECT_CRTC, &crtc_props) ||
        prop_find(fd, &conn_props, "CRTC_ID", &conn_crtc_id, 0) ||
        prop_find(fd, &crtc_props, "MODE_ID", &crtc_mode_id, 0) ||
        prop_find(fd, &crtc_props, "ACTIVE", &crtc_active, 0)) {
        fail("connector/CRTC property discovery", 0);
        free(modes);
        return;
    }

    /* Primary plane: first plane whose "type" property is 1. */
    for (uint32_t i = 0; i < pres.count_planes && !plane_id; i++) {
        if (obj_props(fd, foo[i], DRM_MODE_OBJECT_PLANE, &plane_props))
            continue;
        if (prop_find(fd, &plane_props, "type", 0, &type_val))
            continue;
        if (type_val == 1)
            plane_id = foo[i];
    }
    if (!plane_id ||
        obj_props(fd, plane_id, DRM_MODE_OBJECT_PLANE, &plane_props) ||
        prop_find(fd, &plane_props, "FB_ID", &plane_fb_id, 0) ||
        prop_find(fd, &plane_props, "CRTC_ID", &plane_crtc_id, 0) ||
        prop_find(fd, &plane_props, "SRC_X", &p_src_x, 0) ||
        prop_find(fd, &plane_props, "SRC_Y", &p_src_y, 0) ||
        prop_find(fd, &plane_props, "SRC_W", &p_src_w, 0) ||
        prop_find(fd, &plane_props, "SRC_H", &p_src_h, 0) ||
        prop_find(fd, &plane_props, "CRTC_X", &p_crtc_x, 0) ||
        prop_find(fd, &plane_props, "CRTC_Y", &p_crtc_y, 0) ||
        prop_find(fd, &plane_props, "CRTC_W", &p_crtc_w, 0) ||
        prop_find(fd, &plane_props, "CRTC_H", &p_crtc_h, 0)) {
        fail("primary plane property discovery", 0);
        free(modes);
        return;
    }
    pass("atomic property discovery");

    memset(&cd, 0, sizeof(cd));
    cd.width = 64;
    cd.height = 64;
    cd.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) || !cd.handle) {
        fail("CREATE_DUMB(modeset)", errno);
        free(modes);
        return;
    }

    memset(&fb, 0, sizeof(fb));
    fb.width = 64;
    fb.height = 64;
    fb.pixel_format = DRM_FORMAT_XRGB8888;
    fb.handles[0] = cd.handle;
    fb.pitches[0] = cd.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) || !fb.fb_id) {
        fail("ADDFB2", errno);
        goto out_handle;
    }

    memset(&blob, 0, sizeof(blob));
    blob.data = (uint64_t)(uintptr_t)&modes[0];
    blob.length = sizeof(modes[0]);
    if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) || !blob.blob_id) {
        fail("MODE_CREATEPROPBLOB", errno);
        goto out_fb;
    }

    memset(&req, 0, sizeof(req));
    n = 0;
    props[n] = conn_crtc_id;
    values[n++] = crtc_id;
    req_add(&req, conn_id, props, values, n);
    n = 0;
    props[n] = crtc_mode_id;
    values[n++] = blob.blob_id;
    props[n] = crtc_active;
    values[n++] = 1;
    req_add(&req, crtc_id, props, values, n);
    n = 0;
    props[n] = plane_fb_id;
    values[n++] = fb.fb_id;
    props[n] = plane_crtc_id;
    values[n++] = crtc_id;
    props[n] = p_src_x;
    values[n++] = 0;
    props[n] = p_src_y;
    values[n++] = 0;
    props[n] = p_src_w;
    values[n++] = 64ULL << 16;
    props[n] = p_src_h;
    values[n++] = 64ULL << 16;
    props[n] = p_crtc_x;
    values[n++] = 0;
    props[n] = p_crtc_y;
    values[n++] = 0;
    props[n] = p_crtc_w;
    values[n++] = 64;
    props[n] = p_crtc_h;
    values[n++] = 64;
    req_add(&req, plane_id, props, values, n);

    ret = atomic_commit(fd, &req,
                        DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT);
    if (ret) {
        fail("atomic enable commit", errno);
        goto out_blob;
    }
    pass("atomic enable commit (ALLOW_MODESET)");

    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 1000) <= 0 || !(pfd.revents & POLLIN)) {
        fail("poll for page-flip event", errno);
    } else if (read(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        fail("read page-flip event", errno);
    } else if (ev.base.type == DRM_EVENT_FLIP_COMPLETE) {
        printf("  [PASS] page-flip event via poll/read (seq=%u)\n", ev.sequence);
    } else {
        fail("page-flip event type", 0);
    }

    memset(&vbl, 0, sizeof(vbl));
    vbl.request.type = _DRM_VBLANK_RELATIVE;
    vbl.request.sequence = 1;
    if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl) == 0)
        pass("WAIT_VBLANK relative 1 returned");
    else
        fail("WAIT_VBLANK", errno);

    /* Disable: connector detached, plane off, CRTC MODE_ID/ACTIVE cleared. */
    memset(&req, 0, sizeof(req));
    n = 0;
    props[n] = conn_crtc_id;
    values[n++] = 0;
    req_add(&req, conn_id, props, values, n);
    n = 0;
    props[n] = crtc_mode_id;
    values[n++] = 0;
    props[n] = crtc_active;
    values[n++] = 0;
    req_add(&req, crtc_id, props, values, n);
    n = 0;
    props[n] = plane_fb_id;
    values[n++] = 0;
    props[n] = plane_crtc_id;
    values[n++] = 0;
    req_add(&req, plane_id, props, values, n);

    if (atomic_commit(fd, &req, DRM_MODE_ATOMIC_ALLOW_MODESET) == 0)
        pass("atomic disable commit");
    else
        fail("atomic disable commit", errno);

out_blob:
    memset(&dblob, 0, sizeof(dblob));
    dblob.blob_id = blob.blob_id;
    ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &dblob);
out_fb:
    {
        uint32_t fb_id = fb.fb_id;
        ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
    }
out_handle:
    memset(&ddumb, 0, sizeof(ddumb));
    ddumb.handle = cd.handle;
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &ddumb);
    free(modes);
}

int main(void) {
    int fd;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== AvoryOS Phase 3 KPI DRM test ===\n");

    test_render_node();

    fd = open(CARD_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("  [SKIP] %s not present (%s)\n", CARD_PATH, strerror(errno));
    } else {
        test_gem_mmap(fd);
        test_modeset(fd);
        close(fd);
    }

    printf("\n%s\n", failures ? "=== TESTS FAILED ===" : "=== ALL TESTS PASSED ===");
    return failures ? 1 : 0;
}
