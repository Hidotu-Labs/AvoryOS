// AvoryOS Phase 4 C5 LinuxKPI bochs userland test.
//
// Runs against the imported upstream bochs driver (the TTM canary) on QEMU's
// bochs-display:
//
//   1. discover the bochs card by DRM_IOCTL_VERSION.name (never by minor
//      number), then atomic resource/mode/property discovery.
//   2. CREATE_DUMB sized to the first connector mode (bochs uses the
//      simple-pipe primary plane, which must cover the whole CRTC) ->
//      MAP_DUMB -> mmap -> per-page write/read -> second mapping aliases the
//      same pages -> munmap.
//   3. ADDFB2 + MODE_CREATEPROPBLOB -> atomic enable (ALLOW_MODESET |
//      PAGE_FLIP_EVENT) with full-CRTC rectangles.  bochs has no vblank, so
//      the flip event is read when the helper delivers one; otherwise the
//      step is reported as skipped.
//   4. atomic disable, then blob/FB/dumb cleanup.
//
// Build: userland/test_kpi_bochs.elf, installed as /bin/test_kpi_bochs.  The
// vkms suite (test_kpi_drm) and this one are independent.

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

static int failures;

static void pass(const char *what) { printf("  [PASS] %s\n", what); }

static void fail(const char *what, long detail) {
    failures++;
    printf("  [FAIL] %s", what);
    if (detail)
        printf(" (errno=%ld %s)", detail, strerror((int)detail));
    printf("\n");
}

/* ── card discovery ─────────────────────────────────────────────────────── */

static int find_bochs(void) {
    for (int minor = 0; minor <= 6; minor++) {
        char path[32], name[64];
        struct drm_version ver;
        int fd;

        snprintf(path, sizeof(path), "/dev/dri/card%d", minor);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        memset(&ver, 0, sizeof(ver));
        memset(name, 0, sizeof(name));
        ver.name = name;
        ver.name_len = sizeof(name) - 1;
        if (ioctl(fd, DRM_IOCTL_VERSION, &ver) == 0 && ver.name_len &&
            strncmp(name, "bochs", 5) == 0) {
            printf("  [PASS] bochs card is %s (driver %s)\n", path, name);
            return fd;
        }
        close(fd);
    }
    return -1;
}

/* ── property helpers (same shape as test_kpi_drm) ──────────────────────── */

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

static int obj_props(int fd, uint32_t obj_id, uint32_t obj_type,
                     struct prop_set *out) {
    struct drm_mode_obj_get_properties p;
    uint32_t total;

    memset(&p, 0, sizeof(p));
    p.obj_id = obj_id;
    p.obj_type = obj_type;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p) || !p.count_props)
        return -1;
    total = p.count_props > 64 ? 64 : p.count_props;

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
                     uint32_t *id_out) {
    struct drm_mode_get_property p;

    for (uint32_t i = 0; i < set->count; i++) {
        memset(&p, 0, sizeof(p));
        p.prop_id = set->ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p))
            return -1;
        if (strncmp(p.name, name, sizeof(p.name)) == 0) {
            if (id_out)
                *id_out = set->ids[i];
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

/* ── main suite ─────────────────────────────────────────────────────────── */

static void test_bochs(int fd) {
    struct drm_set_client_cap cap;
    struct drm_mode_card_res res;
    struct drm_mode_get_plane_res pres;
    struct drm_mode_get_connector con;
    struct drm_mode_modeinfo *modes = NULL;
    struct prop_set conn_props, crtc_props, plane_props;
    struct drm_mode_create_dumb cd;
    struct drm_mode_map_dumb md;
    struct drm_mode_fb_cmd2 fb;
    struct drm_mode_create_blob blob;
    struct drm_mode_destroy_blob dblob;
    struct drm_mode_destroy_dumb ddumb;
    struct drm_gem_close gc;
    struct atomic_req req;
    struct drm_event_vblank ev;
    struct pollfd pfd;
    uint32_t foo[16] = {0};
    uint32_t crtc_id = 0, conn_id = 0, plane_id = 0;
    uint32_t conn_crtc_id, crtc_mode_id, crtc_active, plane_fb_id, plane_crtc_id;
    uint32_t p_src_x, p_src_y, p_src_w, p_src_h, p_crtc_x, p_crtc_y, p_crtc_w,
        p_crtc_h;
    uint32_t props[16];
    uint64_t values[16];
    uint32_t mode_cap, n, w, h;
    unsigned char *p1 = MAP_FAILED, *p2 = MAP_FAILED;
    int have_fb = 0, have_blob = 0, have_handle = 0;

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
    w = modes[0].hdisplay;
    h = modes[0].vdisplay;
    if (!w || !h) {
        fail("connector mode", 0);
        free(modes);
        return;
    }
    printf("  [PASS] connector=%u crtc=%u modes=%u (first %ux%u)\n", conn_id,
           crtc_id, con.count_modes, w, h);

    if (obj_props(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, &conn_props) ||
        obj_props(fd, crtc_id, DRM_MODE_OBJECT_CRTC, &crtc_props) ||
        prop_find(fd, &conn_props, "CRTC_ID", &conn_crtc_id) ||
        prop_find(fd, &crtc_props, "MODE_ID", &crtc_mode_id) ||
        prop_find(fd, &crtc_props, "ACTIVE", &crtc_active)) {
        fail("connector/CRTC property discovery", 0);
        free(modes);
        return;
    }

    for (uint32_t i = 0; i < pres.count_planes && !plane_id; i++) {
        uint32_t type_id = 0;

        if (obj_props(fd, foo[i], DRM_MODE_OBJECT_PLANE, &plane_props) ||
            prop_find(fd, &plane_props, "type", &type_id))
            continue;
        for (uint32_t j = 0; j < plane_props.count; j++) {
            if (plane_props.ids[j] == type_id) {
                if (plane_props.vals[j] == 1) /* DRM_PLANE_TYPE_PRIMARY */
                    plane_id = foo[i];
                break;
            }
        }
    }
    if (!plane_id ||
        obj_props(fd, plane_id, DRM_MODE_OBJECT_PLANE, &plane_props) ||
        prop_find(fd, &plane_props, "FB_ID", &plane_fb_id) ||
        prop_find(fd, &plane_props, "CRTC_ID", &plane_crtc_id) ||
        prop_find(fd, &plane_props, "SRC_X", &p_src_x) ||
        prop_find(fd, &plane_props, "SRC_Y", &p_src_y) ||
        prop_find(fd, &plane_props, "SRC_W", &p_src_w) ||
        prop_find(fd, &plane_props, "SRC_H", &p_src_h) ||
        prop_find(fd, &plane_props, "CRTC_X", &p_crtc_x) ||
        prop_find(fd, &plane_props, "CRTC_Y", &p_crtc_y) ||
        prop_find(fd, &plane_props, "CRTC_W", &p_crtc_w) ||
        prop_find(fd, &plane_props, "CRTC_H", &p_crtc_h)) {
        fail("primary plane property discovery", 0);
        free(modes);
        return;
    }
    pass("atomic property discovery");

    /* Full-mode dumb buffer (the simple-pipe plane must cover the CRTC). */
    memset(&cd, 0, sizeof(cd));
    cd.width = w;
    cd.height = h;
    cd.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) || !cd.handle) {
        fail("CREATE_DUMB", errno);
        goto out;
    }
    have_handle = 1;
    printf("  [PASS] CREATE_DUMB handle=%u pitch=%u size=%llu\n", cd.handle,
           cd.pitch, (unsigned long long)cd.size);

    memset(&md, 0, sizeof(md));
    md.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) || !md.offset) {
        fail("MAP_DUMB", errno);
        goto out;
    }
    pass("MAP_DUMB returned an mmap offset");

    p1 = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p1 == MAP_FAILED) {
        fail("mmap(dumb handle)", errno);
        goto out;
    }
    pass("mmap(dumb handle)");

    for (uint32_t i = 0; i < cd.size / 4096; i++) {
        uint32_t *page = (uint32_t *)(p1 + (size_t)i * 4096);
        page[0] = 0xb0000000u ^ i;
    }
    {
        int bad = 0;
        for (uint32_t i = 0; i < cd.size / 4096 && !bad; i++) {
            uint32_t *page = (uint32_t *)(p1 + (size_t)i * 4096);
            if (page[0] != (0xb0000000u ^ i))
                bad = 1;
        }
        if (bad)
            fail("write/read back through the mapping", 0);
        else
            pass("write/read back through the mapping");
    }

    p2 = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p2 == MAP_FAILED) {
        fail("second mmap(dumb handle)", errno);
    } else if (memcmp(p1, p2, cd.size) == 0) {
        pass("second mapping aliases the same pages");
    } else {
        fail("second mapping aliases the same pages", 0);
    }
    if (p2 != MAP_FAILED) {
        p2[4096 + 7] = 0x5A;
        if (p1[4096 + 7] == 0x5A)
            pass("writes through one mapping are visible in the other");
        else
            fail("shared writes between mappings", 0);
        munmap(p2, cd.size);
        p2 = MAP_FAILED;
    }
    munmap(p1, cd.size);
    p1 = MAP_FAILED;
    pass("munmap(dumb handle)");

    memset(&fb, 0, sizeof(fb));
    fb.width = w;
    fb.height = h;
    fb.pixel_format = DRM_FORMAT_XRGB8888;
    fb.handles[0] = cd.handle;
    fb.pitches[0] = cd.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) || !fb.fb_id) {
        fail("ADDFB2", errno);
        goto out;
    }
    have_fb = 1;

    memset(&blob, 0, sizeof(blob));
    blob.data = (uint64_t)(uintptr_t)&modes[0];
    blob.length = sizeof(modes[0]);
    if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) || !blob.blob_id) {
        fail("MODE_CREATEPROPBLOB", errno);
        goto out;
    }
    have_blob = 1;

    /* Atomic enable with full-CRTC rectangles. */
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
    values[n++] = (uint64_t)w << 16;
    props[n] = p_src_h;
    values[n++] = (uint64_t)h << 16;
    props[n] = p_crtc_x;
    values[n++] = 0;
    props[n] = p_crtc_y;
    values[n++] = 0;
    props[n] = p_crtc_w;
    values[n++] = w;
    props[n] = p_crtc_h;
    values[n++] = h;
    req_add(&req, plane_id, props, values, n);

    if (atomic_commit(fd, &req,
                      DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT)) {
        fail("atomic enable commit", errno);
        goto out;
    }
    pass("atomic enable commit (ALLOW_MODESET)");

    /* bochs has no vblank engine; the helper delivers a flip event when it
     * can, otherwise the step is an explicit skip. */
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 300) > 0 && (pfd.revents & POLLIN)) {
        if (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) &&
            ev.base.type == DRM_EVENT_FLIP_COMPLETE)
            printf("  [PASS] page-flip event via poll/read (seq=%u)\n",
                   ev.sequence);
        else
            fail("read page-flip event", errno);
    } else {
        printf("  [SKIP] page-flip event (bochs has no vblank engine)\n");
    }

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

out:
    if (have_blob) {
        memset(&dblob, 0, sizeof(dblob));
        dblob.blob_id = blob.blob_id;
        ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &dblob);
    }
    if (have_fb) {
        uint32_t fb_id = fb.fb_id;
        ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
    }
    if (have_handle) {
        memset(&ddumb, 0, sizeof(ddumb));
        ddumb.handle = cd.handle;
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &ddumb);
        memset(&gc, 0, sizeof(gc));
        gc.handle = cd.handle;
        ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
    free(modes);
}

int main(void) {
    int fd;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== AvoryOS Phase 4 KPI bochs test ===\n");

    fd = find_bochs();
    if (fd < 0) {
        printf("  [SKIP] bochs card not present\n");
    } else {
        test_bochs(fd);
        close(fd);
    }

    printf("\n%s\n", failures ? "=== TESTS FAILED ===" : "=== ALL TESTS PASSED ===");
    return failures ? 1 : 0;
}
