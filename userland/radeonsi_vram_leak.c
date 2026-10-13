/* Diagnostic only: bounded GEM/TTM, RadeonSI and dma-buf lifetime churn.
 * Counters are device-wide, not proof of a leak. Run with other GPU clients idle.
 * No kernel/Mesa policy changes; no deliberate OOM or display modesetting. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <amdgpu_drm.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#define MIB (1024ULL * 1024)
#define SIDE 1024
#define TEX_BYTES (SIDE * SIDE * 4U)
#define RAW_BYTES (4U * 1024 * 1024)
static unsigned cycles = 20, mib = 64, settle_ms = 1000;
static int monitor = -1, errors, incomplete, software;
static char node[128];
static const char *phase = "startup";
static uint64_t bad_bytes;
struct sample { int64_t vram, gtt, visible, available; };
static struct sample baseline, first, last;

static void delay(void) {
    struct timespec t = {settle_ms / 1000, (settle_ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static int info(unsigned query, void *out, unsigned size) {
    if (monitor < 0) { errno = ENODEV; return -1; }
    struct drm_amdgpu_info q = {.return_pointer = (uintptr_t)out,
                               .return_size = size, .query = query};
    return drmIoctl(monitor, DRM_IOCTL_AMDGPU_INFO, &q);
}
static int64_t usage(unsigned query) {
    uint64_t value;
    return info(query, &value, sizeof value) ? -1 : (int64_t)value;
}
static struct sample snapshot(const char *stage, unsigned cycle) {
    struct sample s = {usage(AMDGPU_INFO_VRAM_USAGE), usage(AMDGPU_INFO_GTT_USAGE),
                       usage(AMDGPU_INFO_VIS_VRAM_USAGE), -1};
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256]; unsigned long long kb;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) s.available = kb * 1024;
        fclose(f);
    }
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    printf("MEM\t%s\t%u\t%s\t%lld.%03ld\t%" PRId64 "\t%" PRId64
           "\t%" PRId64 "\t%" PRId64 "\n", phase, cycle, stage,
           (long long)t.tv_sec, t.tv_nsec / 1000000, s.vram, s.gtt, s.visible, s.available);
    return s;
}
static void begin(const char *name) {
    phase = name; delay(); baseline = snapshot("baseline", 0);
    first = last = baseline;
}
static int64_t delta(int64_t a, int64_t b) { return a < 0 || b < 0 ? 0 : a - b; }
static void released(unsigned cycle) {
    delay(); last = snapshot("released", cycle);
    if (cycle == 1) first = last;
}
static void summary(void) {
    printf("TREND phase=%s released_last_minus_first vram=%" PRId64
           " gtt=%" PRId64 " available=%" PRId64 " bytes; missing counters have no delta\n",
           phase, delta(last.vram, first.vram), delta(last.gtt, first.gtt),
           delta(last.available, first.available));
    printf("DELTA phase=%s final_minus_baseline vram=%" PRId64 " gtt=%" PRId64
           " available=%" PRId64 " bytes\n", phase, delta(last.vram, baseline.vram),
           delta(last.gtt, baseline.gtt), delta(last.available, baseline.available));
}
static int error(const char *step) {
    fprintf(stderr, "ERROR phase=%s step=%s errno=%d (%s) EGL=0x%x\n", phase, step, errno, strerror(errno), eglGetError());
    errors++; return -1;
}
static int gl_ok(const char *step) {
    GLenum e = glGetError();
    if (!e) return 1;
    fprintf(stderr, "ERROR phase=%s step=%s GL=0x%x\n", phase, step, e); errors++; return 0;
}
static int open_amd(const char *path) {
    char name[32] = {0};
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct drm_version v = {.name = name, .name_len = sizeof name - 1};
    if (!drmIoctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, "amdgpu")) return fd;
    close(fd); errno = ENODEV; return -1;
}
struct raw_bo { uint32_t handle, imported; int dma; void *map; };
static void raw_phase(unsigned domain, int prime) {
    unsigned count = mib / 4;
    struct raw_bo *bos = calloc(count, sizeof *bos);
    if (!bos) { error("CPU bookkeeping allocation"); return; }
    begin(prime ? "raw-prime" : domain == AMDGPU_GEM_DOMAIN_VRAM ? "raw-vram" : "raw-gtt");
    for (unsigned c = 1; c <= cycles; c++) {
        int owner = open_amd(node), importer = prime ? open_amd(node) : -1;
        int failed = 0;
        for (unsigned i = 0; i < count; i++) bos[i] = (struct raw_bo){.dma = -1, .map = MAP_FAILED};
        if (owner < 0 || (prime && importer < 0)) { error("open churn DRM file"); failed = 1; }
        for (unsigned i = 0; i < count && !failed; i++) {
            union drm_amdgpu_gem_create create = {0};
            create.in.bo_size = RAW_BYTES; create.in.alignment = 4096;
            create.in.domains = domain;
            create.in.domain_flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
            if (drmIoctl(owner, DRM_IOCTL_AMDGPU_GEM_CREATE, &create)) { error("GEM create"); failed = 1; break; }
            bos[i].handle = create.out.handle;
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            union drm_amdgpu_gem_wait_idle idle = {0};
            idle.in.handle = bos[i].handle;
            idle.in.timeout = (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec + 5000000000ULL;
            if (drmIoctl(owner, DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE, &idle) || idle.out.status) {
                error("GEM idle/initialization fence (5 second deadline)"); failed = 1; break;
            }
            union drm_amdgpu_gem_mmap map = {0}; map.in.handle = bos[i].handle;
            if (drmIoctl(owner, DRM_IOCTL_AMDGPU_GEM_MMAP, &map)) { error("GEM mmap ioctl"); failed = 1; break; }
            bos[i].map = mmap(NULL, RAW_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, owner, map.out.addr_ptr);
            if (bos[i].map == MAP_FAILED) { error("GEM mmap"); failed = 1; break; }
            uint32_t *p = bos[i].map;
            for (unsigned w = 0; w < RAW_BYTES / 4; w++) p[w] = 0x9e3779b9U * (w + 1) ^ (c * 65537U + i);
            if (prime && (drmPrimeHandleToFD(owner, bos[i].handle, DRM_CLOEXEC | DRM_RDWR, &bos[i].dma) ||
                          drmPrimeFDToHandle(importer, bos[i].dma, &bos[i].imported))) {
                error("PRIME export/import"); failed = 1; break;
            }
        }
        snapshot("live", c);
        uint64_t bad = 0;
        for (unsigned i = 0; i < count; i++) {
            if (bos[i].map == MAP_FAILED) continue;
            volatile uint32_t *p = bos[i].map;
            for (unsigned w = 0; w < RAW_BYTES / 4; w++) {
                uint32_t want = 0x9e3779b9U * (w + 1) ^ (c * 65537U + i);
                if (p[w] != want) {
                    if (bad < 4) printf("MISMATCH phase=%s cycle=%u bo=%u word=%u got=%08x want=%08x\n",
                                        phase, c, i, w, p[w], want);
                    bad += 4;
                }
            }
            if (munmap(bos[i].map, RAW_BYTES)) error("munmap");
        }
        bad_bytes += bad;
        if (bad) errors++;
        /* Alternate explicit GEM_CLOSE and implicit file-close release. */
        if (c & 1) for (unsigned i = 0; i < count; i++) {
            struct drm_gem_close close_bo = {.handle = bos[i].handle};
            if (close_bo.handle && drmIoctl(owner, DRM_IOCTL_GEM_CLOSE, &close_bo)) error("GEM close owner");
        }
        if (owner >= 0 && close(owner)) error("close owner DRM file");
        if (importer >= 0 && close(importer)) error("close importer DRM file");
        if (prime) snapshot("dma_fds_only", c); /* Retention here is expected. */
        for (unsigned i = 0; i < count; i++) if (bos[i].dma >= 0 && close(bos[i].dma)) error("close dma-buf fd");
        printf("CHECK phase=%s cycle=%u mismatch_bytes=%" PRIu64 "\n", phase, c, bad);
        released(c);
        if (failed || bad || errors) break;
    }
    summary(); free(bos);
}
static GLuint shader(GLenum kind, const char *source) {
    GLuint s = glCreateShader(kind); glShaderSource(s, 1, &source, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "SHADER %s\n", log); errors++; }
    return s;
}
static void pattern(unsigned char *p, unsigned seed) {
    for (unsigned y = 0; y < SIDE; y++) for (unsigned x = 0; x < SIDE; x++) {
        size_t n = ((size_t)y * SIDE + x) * 4;
        p[n] = x * 13 + y * 7 + seed * 31;
        p[n+1] = x * 3 + y * 19 + seed * 47;
        p[n+2] = (x ^ y) + seed * 61; p[n+3] = 255;
    }
}
struct shared_bo { struct gbm_bo *bo; EGLImageKHR image; int fd; };
static void gl_phase(int prime) {
    begin(software ? "software-gl" : prime ? "egl-prime" : "gl-textures");
    int fd = software ? -1 : open_amd(node);
    struct gbm_device *gbm = fd >= 0 ? gbm_create_device(fd) : NULL;
    EGLDisplay dpy = EGL_NO_DISPLAY; EGLContext ctx = EGL_NO_CONTEXT;
    unsigned count = mib / 4;
    GLuint *textures = calloc(count, sizeof *textures), output = 0, fbo = 0, program = 0, vs = 0, fs = 0;
    struct shared_bo *shared = calloc(count, sizeof *shared);
    unsigned char *upload = malloc(TEX_BYTES), *readback = malloc(TEX_BYTES);
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLCREATEIMAGEKHRPROC image_create = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC image_destroy = (void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_bind = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if ((!software && !gbm) || !platform || !textures || !shared || !upload || !readback) { error("GL setup allocations"); goto out; }
    dpy = platform(software ? EGL_PLATFORM_SURFACELESS_MESA : EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint major, minor, n; EGLConfig config;
    EGLint attrs[] = {EGL_SURFACE_TYPE, software ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    if (!eglInitialize(dpy, &major, &minor) || !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(dpy, attrs, &config, 1, &n) || n != 1) { error("EGL initialize/config"); goto out; }
    ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { error("EGL context"); goto out; }
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("RENDERER %s GL=%s\n", renderer ? renderer : "(null)", glGetString(GL_VERSION));
    if (!renderer || (software ? (!strstr(renderer, "llvmpipe") && !strstr(renderer, "softpipe")) : !strstr(renderer, "radeonsi"))) {
        fprintf(stderr, "INCOMPLETE unexpected renderer for %s control\n", software ? "software" : "RadeonSI");
        incomplete++; goto out;
    }
    const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
    if (prime && (!image_create || !image_destroy || !image_bind || !ext || !strstr(ext, "EGL_EXT_image_dma_buf_import"))) {
        printf("INCOMPLETE EGL dma-buf import unavailable\n"); incomplete++; goto out;
    }
    vs = shader(GL_VERTEX_SHADER, "#version 300 es\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));gl_Position=vec4(p*2.0-1.0,0,1);}");
    fs = shader(GL_FRAGMENT_SHADER, "#version 300 es\nprecision highp float;uniform sampler2D src;out vec4 color;void main(){color=texelFetch(src,ivec2(gl_FragCoord.xy),0);}");
    program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
    GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked || errors) { error("GL shader program"); goto out; }
    glUseProgram(program); glUniform1i(glGetUniformLocation(program, "src"), 0);
    glDisable(GL_DITHER); glDisable(GL_BLEND); glViewport(0, 0, SIDE, SIDE);
    glGenTextures(1, &output); glBindTexture(GL_TEXTURE_2D, output);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, SIDE, SIDE);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || !gl_ok("output FBO")) { error("output FBO"); goto out; }
    snapshot("context_ready", 0);
    for (unsigned c = 1; c <= cycles; c++) {
        glGenTextures(count, textures);
        for (unsigned i = 0; i < count; i++) shared[i] = (struct shared_bo){.fd = -1, .image = EGL_NO_IMAGE_KHR};
        unsigned made = 0;
        for (unsigned i = 0; i < count; i++) {
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            if (prime) {
                shared[i].bo = gbm_bo_create(gbm, SIDE, SIDE, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
                if (!shared[i].bo) { error("linear GBM BO"); break; }
                shared[i].fd = gbm_bo_get_fd(shared[i].bo);
                if (shared[i].fd < 0) { error("GBM export"); break; }
                EGLint ia[] = {EGL_WIDTH, SIDE, EGL_HEIGHT, SIDE, EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_ABGR8888,
                    EGL_DMA_BUF_PLANE0_FD_EXT, shared[i].fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(shared[i].bo), EGL_NONE};
                shared[i].image = image_create(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
                if (shared[i].image == EGL_NO_IMAGE_KHR) { error("EGL dma-buf import"); break; }
                image_bind(GL_TEXTURE_2D, shared[i].image);
            } else glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, SIDE, SIDE);
            pattern(upload, c * 257 + i);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, upload);
            if (!gl_ok("texture allocation/upload")) break;
            made++;
        }
        glFinish(); gl_ok("live allocation fence"); snapshot("live", c);
        uint64_t bad = 0;
        /* Keep the whole batch alive and sample every texture via a GPU shader. */
        for (unsigned i = 0; i < made && !errors; i++) {
            pattern(upload, c * 257 + i); glBindTexture(GL_TEXTURE_2D, textures[i]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, readback);
            if (!gl_ok("shader/readback")) break;
            for (size_t b = 0; b < TEX_BYTES; b++) if (upload[b] != readback[b]) {
                if (bad < 4) printf("MISMATCH phase=%s cycle=%u texture=%u byte=%zu got=%u want=%u\n", phase, c, i, b, readback[b], upload[b]);
                bad++;
            }
        }
        glFinish(); glBindTexture(GL_TEXTURE_2D, 0); glDeleteTextures(count, textures);
        memset(textures, 0, count * sizeof *textures);
        for (unsigned i = 0; i < count; i++) {
            if (shared[i].image != EGL_NO_IMAGE_KHR && !image_destroy(dpy, shared[i].image)) error("EGL image destroy");
            if (shared[i].fd >= 0 && close(shared[i].fd)) error("close GBM dma-buf");
            if (shared[i].bo) gbm_bo_destroy(shared[i].bo);
        }
        glFinish(); gl_ok("release fence"); bad_bytes += bad;
        printf("CHECK phase=%s cycle=%u checked_textures=%u mismatch_bytes=%" PRIu64 "\n", phase, c, made, bad);
        if (bad) errors++;
        released(c); if (errors) break;
    }
    summary();
out:
    if (ctx != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() == ctx) {
            if (fbo) glDeleteFramebuffers(1, &fbo);
            if (output) glDeleteTextures(1, &output);
            if (program) glDeleteProgram(program);
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            glFinish();
        }
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (!eglDestroyContext(dpy, ctx)) error("EGL context destroy");
    }
    if (dpy != EGL_NO_DISPLAY && !eglTerminate(dpy)) error("EGL terminate");
    if (gbm) gbm_device_destroy(gbm);
    if (fd >= 0 && close(fd)) error("close GL DRM file");
    free(textures); free(shared); free(upload); free(readback);
    delay();
    struct sample teardown = snapshot("context_destroyed", cycles);
    printf("TEARDOWN phase=%s final_minus_baseline vram=%" PRId64 " gtt=%" PRId64
           " available=%" PRId64 " bytes\n", phase, delta(teardown.vram, baseline.vram),
           delta(teardown.gtt, baseline.gtt), delta(teardown.available, baseline.available));
}
static void help(const char *prog) {
    printf("Usage: %s [--phase all|raw-vram|raw-gtt|raw-prime|gl|prime] [--cycles N] [--mib N] [--settle-ms N] [--node PATH] [--software]\n"
           "Defaults: all, 20 cycles, 64 MiB live payload, 1000 ms release wait.\n"
           "MiB must be 4..1024, a multiple of 4. Internal driver/staging overhead is additional.\n"
           "MEM columns: phase cycle stage monotonic_seconds vram_bytes gtt_bytes visible_bytes memavailable_bytes. -1 means unavailable.\n"
           "--software runs only the GL pixel oracle on llvmpipe; no GPU leak diagnosis.\n"
           "Exit: 0 completed (inspect trends), 1 runtime/pixel failure, 2 invalid setup or incomplete.\n", prog);
}
static int number(const char *s, unsigned *v, unsigned min, unsigned max) {
    char *end; errno = 0; unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || n < min || n > max) return 0;
    *v = n; return 1;
}
int main(int argc, char **argv) {
    const char *selected = "all";
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--help")) { help(argv[0]); return 0; }
        if (!strcmp(argv[a], "--software")) { software = 1; continue; }
        if (a + 1 == argc) { help(argv[0]); return 2; }
        const char *key = argv[a++], *value = argv[a];
        int ok = 1;
        if (!strcmp(key, "--cycles")) ok = number(value, &cycles, 1, 100000);
        else if (!strcmp(key, "--mib")) ok = number(value, &mib, 4, 1024) && mib % 4 == 0;
        else if (!strcmp(key, "--settle-ms")) ok = number(value, &settle_ms, 0, 60000);
        else if (!strcmp(key, "--phase")) selected = value;
        else if (!strcmp(key, "--node")) { if (strlen(value) >= sizeof node) ok = 0; else strcpy(node, value); }
        else ok = 0;
        if (!ok) { help(argv[0]); return 2; }
    }
    if (strcmp(selected, "all") && strcmp(selected, "raw-vram") && strcmp(selected, "raw-gtt") &&
        strcmp(selected, "raw-prime") && strcmp(selected, "gl") && strcmp(selected, "prime")) { help(argv[0]); return 2; }
    if (software) {
        if (strcmp(selected, "all") && strcmp(selected, "gl")) { help(argv[0]); return 2; }
        selected = "gl";
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
        /* GLVND can otherwise select a non-Mesa vendor despite this flag. */
        const char *vendor = "/usr/share/glvnd/egl_vendor.d/50_mesa.json";
        if (!access(vendor, R_OK)) setenv("__EGL_VENDOR_LIBRARY_FILENAMES", vendor, 1);
    }
    if (software) strcpy(node, "software-control");
    else if (*node) monitor = open_amd(node);
    else for (int n = 128; n < 192; n++) {
        snprintf(node, sizeof node, "/dev/dri/renderD%d", n); monitor = open_amd(node); if (monitor >= 0) break;
    }
    if (!software && monitor < 0) { fprintf(stderr, "ERROR: no amdgpu render node\n"); return 2; }
    printf("radeonsi_vram_leak v1 node=%s cycles=%u payload_mib=%u settle_ms=%u phase=%s\n", node, cycles, mib, settle_ms, selected);
    printf("NOTE counters include all GPU clients; caching and delayed release can retain memory. This run does not deliberately exhaust VRAM.\n");
    struct drm_amdgpu_memory_info heaps = {0};
    if (!info(AMDGPU_INFO_MEMORY, &heaps, sizeof heaps))
        printf("HEAPS vram_total=%llu visible_total=%llu gtt_total=%llu bytes\n", (unsigned long long)heaps.vram.total_heap_size,
               (unsigned long long)heaps.cpu_accessible_vram.total_heap_size, (unsigned long long)heaps.gtt.total_heap_size);
    struct sample start = snapshot("start", 0);
    if (!software && (start.vram < 0 || start.gtt < 0)) { puts("INCOMPLETE VRAM/GTT usage counters unavailable; leak trends cannot be established"); incomplete++; }
    int all = !strcmp(selected, "all");
    if (all || !strcmp(selected, "raw-vram")) raw_phase(AMDGPU_GEM_DOMAIN_VRAM, 0);
    if (!errors && (all || !strcmp(selected, "raw-gtt"))) raw_phase(AMDGPU_GEM_DOMAIN_GTT, 0);
    if (!errors && (all || !strcmp(selected, "raw-prime"))) raw_phase(AMDGPU_GEM_DOMAIN_VRAM, 1);
    if (!errors && (all || !strcmp(selected, "gl"))) gl_phase(0);
    if (!errors && (all || !strcmp(selected, "prime"))) gl_phase(1);
    phase = "final"; delay(); snapshot("end", 0);
    if (monitor >= 0) close(monitor);
    printf("RESULT errors=%d incomplete=%d mismatch_bytes=%" PRIu64 "; %s\n", errors, incomplete, bad_bytes,
           errors ? "FAILED" : software ? "software pixel oracle only; hardware lifetime phases did not run" : incomplete ? "INCOMPLETE" : "workload completed; inspect MEM/TREND/DELTA for retention, not an automatic no-leak verdict");
    return errors ? 1 : incomplete ? 2 : 0;
}
