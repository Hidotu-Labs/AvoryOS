/* Diagnostic only: unmodified Mesa, headless RadeonSI, no game preferences.
 * Texture/mipmap uploads and explicit UBO/SSBO submissions have CPU oracles.
 * The serialized UBO case is a control in this process, not a driver fix. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef PIPELINE_DESKTOP
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#else
#include <GLES3/gl31.h>
#endif
#include <gbm.h>

static int software, failures, inconclusive;

static void fatal(const char *step) {
    fprintf(stderr, "ERROR: %s; GL=0x%x EGL=0x%x (checks incomplete)\n",
            step, glGetError(), eglGetError());
    exit(2);
}
static void gl_check(const char *step) {
    GLenum error = glGetError();
    if (error) { fprintf(stderr, "ERROR: %s GL=0x%x\n", step, error); exit(2); }
}
static GLuint compile(GLenum type, const char *code) {
    GLuint s = glCreateShader(type);
#ifdef PIPELINE_DESKTOP
    /* Keep the workload/oracles identical across GLES and desktop GL. */
    const char *body = strchr(code, '\n');
    if (!body) fatal("missing shader version line");
    const char *parts[] = {"#version 430 core\n", body + 1};
    glShaderSource(s, 2, parts, NULL); glCompileShader(s);
#else
    glShaderSource(s, 1, &code, NULL); glCompileShader(s);
#endif
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "ERROR: shader compilation: %s\n", log); exit(2);
    }
    return s;
}
static GLuint link_program(GLuint first, GLuint second) {
    GLuint p = glCreateProgram(); glAttachShader(p, first);
    if (second) glAttachShader(p, second);
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "ERROR: program link: %s\n", log); exit(2);
    }
    return p;
}
static int amd_node(void) {
    for (int n = 128; n < 160; n++) {
        char path[64], name[32] = {0};
        snprintf(path, sizeof path, "/dev/dri/renderD%d", n);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct drm_version v = {.name = name, .name_len = sizeof name - 1};
        if (!drmIoctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, "amdgpu")) return fd;
        close(fd);
    }
    return -1;
}
static void texel(unsigned x, unsigned y, unsigned seed, unsigned level,
                  int filtered, unsigned char rgba[4]) {
    rgba[0] = ((filtered ? 0 : x * 13 + y * 7) + seed * 31 + level * 17) & 255;
    rgba[1] = ((filtered ? 0 : (x >> 2) * 19 + y * 11) + seed * 47 + level * 29) & 255;
    rgba[2] = ((filtered ? 0 : x * 3 + (y >> 1) * 23) + seed * 61 + level * 43) & 255;
    rgba[3] = filtered || ((x ^ y ^ seed) & 1) ? 255 : 0;
}

/* Known mip levels isolate both source texels and mip selection. The filtered
 * case uses spatially constant levels: trilinear rounding has a small oracle
 * tolerance without ambiguous alpha-discard thresholds or texture edges. */
static void texture_case(int filtered) {
    enum { SOURCE = 512, LEVELS = 10, GRID = 8, TILE = 64, SIDE = GRID * TILE, SOURCES = 8 };
    unsigned batches = software ? 1 : 8;
    unsigned char *upload = malloc(SOURCE * SOURCE * 4);
    unsigned char *pixels = malloc(SIDE * SIDE * 4);
    if (!upload || !pixels) fatal("texture CPU allocation");
    GLuint vs = compile(GL_VERTEX_SHADER,
        "#version 300 es\nlayout(location=0) in vec2 pos; out vec2 uv;"
        "void main(){uv=(pos+1.0)*0.5;gl_Position=vec4(pos,0.0,1.0);}");
    GLuint fs = compile(GL_FRAGMENT_SHADER,
        "#version 300 es\nprecision highp float; in vec2 uv; out vec4 out_color;"
        "uniform sampler2D tex; uniform float lod_value,distance_value,density;"
        "void main(){vec2 biased=uv+vec2(0.25/exp2(9.0-floor(lod_value)));"
        "vec4 c=textureLod(tex,biased,lod_value);if(c.a<0.5)discard;"
        "float f=exp(-density*density*distance_value*distance_value);"
        "out_color=vec4(mix(vec3(0.2,0.4,0.6),c.rgb,f),1.0);}");
    GLuint program = link_program(vs, fs); glUseProgram(program);
    GLint lod_loc = glGetUniformLocation(program, "lod_value");
    GLint distance_loc = glGetUniformLocation(program, "distance_value");
    GLint density_loc = glGetUniformLocation(program, "density");
    glUniform1i(glGetUniformLocation(program, "tex"), 0);
    glUniform1f(density_loc, filtered ? 0.035f : 0.0f);
    GLuint vbo, vao, output, fbo, sources[SOURCES];
    GLfloat quad[] = {-1,-1, 1,-1, -1,1, 1,1};
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glActiveTexture(GL_TEXTURE0); glGenTextures(1, &output); glBindTexture(GL_TEXTURE_2D, output);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, SIDE, SIDE);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fatal("texture framebuffer");
    glGenTextures(SOURCES, sources);
    for (unsigned i = 0; i < SOURCES; i++) {
        glBindTexture(GL_TEXTURE_2D, sources[i]);
        glTexStorage2D(GL_TEXTURE_2D, LEVELS, GL_RGBA8, SOURCE, SOURCE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                         filtered ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtered ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glDisable(GL_BLEND); glDisable(GL_DITHER); glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    unsigned long long bad = 0, checked = 0;
    const char *name = filtered ? "trilinear-mips+EXP2-fog" : "texture-reuse+mips+alpha-discard";
    printf("BEGIN %s: %u batches, %u source textures\n", name, batches, SOURCES); fflush(stdout);
    for (unsigned batch = 0; batch < batches; batch++) {
        glClearColor(7.0f/255, 13.0f/255, 23.0f/255, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        for (unsigned draw = 0; draw < GRID * GRID; draw++) {
            unsigned seed = draw + batch * GRID * GRID;
            glBindTexture(GL_TEXTURE_2D, sources[draw % SOURCES]);
            for (unsigned level = 0; level < LEVELS; level++) {
                unsigned size = SOURCE >> level;
                for (unsigned y = 0; y < size; y++) for (unsigned x = 0; x < size; x++)
                    texel(x, y, seed, level, filtered, upload + (y * size + x) * 4);
                glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, upload);
            }
            glViewport((draw % GRID) * TILE, (draw / GRID) * TILE, TILE, TILE);
            glUniform1f(lod_loc, (float)(draw % (LEVELS - 1)) + (filtered ? 0.25f : 0));
            glUniform1f(distance_loc, (float)(draw % 17) + 0.5f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFlush(); /* No per-draw finish/readback; later uploads reuse sources. */
        }
        glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        gl_check(name);
        for (unsigned y = 0; y < SIDE; y++) for (unsigned x = 0; x < SIDE; x++) {
            unsigned draw = (y / TILE) * GRID + x / TILE;
            unsigned seed = draw + batch * GRID * GRID, level = draw % (LEVELS - 1);
            unsigned size = SOURCE >> level;
            /* Quarter-texel bias avoids exact nearest-selection boundaries. */
            unsigned sx = ((2 * (x % TILE) + 1) * size * 2 + TILE) / (4 * TILE);
            unsigned sy = ((2 * (y % TILE) + 1) * size * 2 + TILE) / (4 * TILE);
            if (sx >= size) sx = size - 1;
            if (sy >= size) sy = size - 1;
            unsigned char c[4], next[4]; texel(sx, sy, seed, level, filtered, c);
            float expected[4] = {7, 13, 23, 255};
            if (c[3]) {
                texel(0, 0, seed, level + 1, filtered, next);
                float distance = (float)(draw % 17) + 0.5f;
                float f = filtered ? expf(-0.035f * 0.035f * distance * distance) : 1.0f;
                for (unsigned k = 0; k < 3; k++) {
                    float sampled = filtered ? c[k] * 0.75f + next[k] * 0.25f : c[k];
                    expected[k] = (float)(k + 1) * 51 * (1 - f) + sampled * f;
                }
            }
            unsigned char *actual = pixels + (y * SIDE + x) * 4;
            int wrong = 0;
            for (unsigned k = 0; k < 4; k++)
                if (fabsf(actual[k] - expected[k]) > (filtered ? 2.1f : 0.1f)) wrong = 1;
            if (wrong) {
                if (bad < 8) fprintf(stderr,
                    "MISMATCH %s batch=%u tile=%u level=%u pixel=%u,%u expected=%.1f,%.1f,%.1f,%.1f actual=%u,%u,%u,%u\n",
                    name, batch, draw, level, x, y, expected[0], expected[1], expected[2], expected[3],
                    actual[0], actual[1], actual[2], actual[3]);
                bad++;
            }
            checked++;
        }
    }
    printf("%s %s: pixels=%llu mismatches=%llu\n", bad ? "FAIL" : "PASS", name, checked, bad);
    failures += !!bad; fflush(stdout);
    glDeleteTextures(SOURCES, sources); glDeleteTextures(1, &output);
    glDeleteFramebuffers(1, &fbo); glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs); free(upload); free(pixels);
}

static uint32_t lcg(uint32_t value, unsigned rounds) {
    uint32_t mul = 1664525u, add = 1013904223u, product = 1, offset = 0;
    while (rounds) {
        if (rounds & 1) { offset = offset * mul + add; product *= mul; }
        add *= mul + 1; mul *= mul; rounds >>= 1;
    }
    return product * value + offset;
}
static void ubo_case(int fresh, int serial) {
    enum { WORDS = 4096, VALUES = 256 };
    unsigned trials = software ? 2 : 64, rounds = software ? 64 : 32768;
    const char *name = serial ? "UBO-serialized-control" : fresh ? "UBO-fresh-descriptors" : "UBO-reused-buffer";
    GLuint hs = compile(GL_COMPUTE_SHADER,
        "#version 310 es\nprecision highp int; layout(local_size_x=64) in;"
        "layout(std430,binding=0) buffer Result{uint result[];};"
        "uniform uint rounds,work_seed; void main(){uint i=gl_GlobalInvocationID.x;"
        "uint v=(i+1u)^work_seed;for(uint n=0u;n<rounds;n++)v=v*1664525u+1013904223u;result[i]=v;}");
    GLuint ls = compile(GL_COMPUTE_SHADER,
        "#version 310 es\nprecision highp int; layout(local_size_x=64) in;"
        "layout(std140,binding=0) uniform Pattern{uvec4 uploaded;};"
        "layout(std430,binding=1) readonly buffer RawInput{uint raw[];};"
        "layout(std430,binding=0) buffer Result{uvec4 result[];};"
        "void main(){uint i=gl_GlobalInvocationID.x,j=i&3u;uvec4 mask=uvec4(i*17u,i*31u,i*53u,i*71u);"
        "result[i*2u]=uploaded^mask;"
        "result[i*2u+1u]=uvec4(raw[j],raw[(j+1u)&3u],raw[(j+2u)&3u],raw[(j+3u)&3u])^mask;}");
    GLuint heavy = link_program(hs, 0), light = link_program(ls, 0);
    GLuint heavy_out, light_out, ubo = 0;
    glGenBuffers(1, &heavy_out); glBindBuffer(GL_SHADER_STORAGE_BUFFER, heavy_out);
    glBufferData(GL_SHADER_STORAGE_BUFFER, WORDS * 4, NULL, GL_DYNAMIC_COPY);
    glGenBuffers(1, &light_out); glBindBuffer(GL_SHADER_STORAGE_BUFFER, light_out);
    glBufferData(GL_SHADER_STORAGE_BUFFER, VALUES * 32, NULL, GL_DYNAMIC_COPY);
    if (!fresh) {
        glGenBuffers(1, &ubo); glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, 16, NULL, GL_DYNAMIC_DRAW);
    }
    unsigned bad_trials = 0, storage_bad = 0, cpu_bad = 0, pending = 0, heavy_bad = 0;
    printf("BEGIN %s: %u trials, %u dependent work rounds\n", name, trials, rounds); fflush(stdout);
    for (unsigned trial = 0; trial < trials; trial++) {
        uint32_t seed = trial * 97 + 11;
        uint32_t value[4] = {0x12345678u ^ trial, 0x9abcdef0u + trial * 17,
                             0xfedcba98u - trial * 31, 0x87654321u ^ (trial * 53)};
        glUseProgram(heavy); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, heavy_out);
        glUniform1ui(glGetUniformLocation(heavy, "rounds"), rounds);
        glUniform1ui(glGetUniformLocation(heavy, "work_seed"), seed);
        glDispatchCompute(WORDS / 64, 1, 1);
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); glFlush();
        if (serial) glFinish();
        if (fresh) {
            glGenBuffers(1, &ubo); glBindBuffer(GL_UNIFORM_BUFFER, ubo);
            glBufferData(GL_UNIFORM_BUFFER, 16, value, GL_DYNAMIC_DRAW);
        } else {
            glBindBuffer(GL_UNIFORM_BUFFER, ubo);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, 16, value);
        }
        glUseProgram(light); glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, light_out);
        /* Same object read through UBO and divergent SSBO addresses. This
         * distinguishes scalar-uniform reads from storage-memory reads. */
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ubo);
        GLenum state = glClientWaitSync(fence, 0, 0); /* Nonblocking observation. */
        if (state == GL_WAIT_FAILED) fatal("heavy fence query");
        pending += state == GL_TIMEOUT_EXPIRED;
        glDispatchCompute(VALUES / 64, 1, 1); glFlush();
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, light_out);
        const uint32_t *actual = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, VALUES * 32, GL_MAP_READ_BIT);
        if (!actual) fatal("UBO result readback");
        int wrong = 0, storage_wrong = 0;
        const unsigned multipliers[4] = {17, 31, 53, 71};
        for (unsigned i = 0; i < VALUES; i++) for (unsigned k = 0; k < 4; k++) {
            uint32_t expected = value[k] ^ (i * multipliers[k]);
            if (actual[i * 8 + k] != expected) {
                if (!wrong && bad_trials < 8) fprintf(stderr,
                    "MISMATCH %s trial=%u index=%u channel=%u uploaded=%08x expected=%08x actual=%08x previous_fence_pending=%d\n",
                    name, trial, i, k, value[k], expected, actual[i * 8 + k], state == GL_TIMEOUT_EXPIRED);
                wrong = 1;
            }
            expected = value[(i + k) & 3] ^ (i * multipliers[k]);
            if (actual[i * 8 + 4 + k] != expected) {
                if (!storage_wrong && storage_bad < 8) fprintf(stderr,
                    "MISMATCH storage-read %s trial=%u index=%u channel=%u expected=%08x actual=%08x\n",
                    name, trial, i, k, expected, actual[i * 8 + 4 + k]);
                storage_wrong = 1;
            }
        }
        bad_trials += wrong;
        storage_bad += storage_wrong;
        if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)) fatal("UBO result unmap");
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        actual = glMapBufferRange(GL_UNIFORM_BUFFER, 0, 16, GL_MAP_READ_BIT);
        if (!actual) fatal("UBO CPU source readback");
        if (memcmp(actual, value, sizeof value)) {
            if (cpu_bad < 8) fprintf(stderr,"MISMATCH CPU-source %s trial=%u expected=%08x,%08x,%08x,%08x actual=%08x,%08x,%08x,%08x\n",
                name, trial, value[0],value[1],value[2],value[3],actual[0],actual[1],actual[2],actual[3]);
            cpu_bad++;
        }
        if (!glUnmapBuffer(GL_UNIFORM_BUFFER)) fatal("UBO CPU source unmap");
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, heavy_out);
        actual = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, WORDS * 4, GL_MAP_READ_BIT);
        if (!actual) fatal("heavy result readback");
        for (unsigned i = 0; i < WORDS; i++) {
            uint32_t expected = lcg((i + 1) ^ seed, rounds);
            if (actual[i] != expected) {
                if (heavy_bad < 8) fprintf(stderr,"MISMATCH heavy-work %s trial=%u index=%u expected=%08x actual=%08x\n",
                                           name, trial, i, expected, actual[i]);
                heavy_bad++; break;
            }
        }
        if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)) fatal("heavy result unmap");
        glDeleteSync(fence);
        if (fresh) { glDeleteBuffers(1, &ubo); ubo = 0; }
        gl_check(name);
    }
    printf("%s %s: uniform_bad=%u/%u storage_bad=%u cpu_source_bad=%u heavy_bad=%u previous_fence_pending=%u/%u\n",
           bad_trials || storage_bad || cpu_bad || heavy_bad ? "FAIL" : "PASS",
           name, bad_trials, trials, storage_bad, cpu_bad, heavy_bad, pending, trials);
    if (!serial && !pending && !bad_trials && !storage_bad && !cpu_bad && !heavy_bad) {
        printf("INCONCLUSIVE %s: no pending previous fence was observed\n", name); inconclusive++;
    }
    failures += !!(bad_trials || storage_bad || cpu_bad || heavy_bad); fflush(stdout);
    if (ubo) glDeleteBuffers(1, &ubo);
    glDeleteBuffers(1, &heavy_out); glDeleteBuffers(1, &light_out);
    glDeleteProgram(heavy); glDeleteProgram(light); glDeleteShader(hs); glDeleteShader(ls);
}

int main(int argc, char **argv) {
    software = argc == 2 && !strcmp(argv[1], "--software");
    if (argc != 1 && !software) { fprintf(stderr, "Usage: %s [--software]\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int fd = software ? -1 : amd_node();
    if (!software && fd < 0) { fputs("ERROR: no AMD render node; checks did not run\n", stderr); return 2; }
    struct gbm_device *gbm = software ? NULL : gbm_create_device(fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!platform || (!software && !gbm)) fatal("GBM/platform setup");
    if (software) setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    EGLDisplay dpy = platform(software ? EGL_PLATFORM_SURFACELESS_MESA : EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint major, minor, count;
#ifdef PIPELINE_DESKTOP
    const EGLenum api = EGL_OPENGL_API;
    const EGLint renderable = EGL_OPENGL_BIT;
    EGLint ctxattrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
    const char *api_name = "desktop-OpenGL";
#else
    const EGLenum api = EGL_OPENGL_ES_API;
    const EGLint renderable = EGL_OPENGL_ES3_BIT_KHR;
    EGLint ctxattrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE};
    const char *api_name = "OpenGL-ES";
#endif
    if (!eglInitialize(dpy, &major, &minor) || !eglBindAPI(api)) fatal("EGL initialize");
    EGLint attrs[] = {EGL_SURFACE_TYPE, software ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, renderable, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config;
    if (!eglChooseConfig(dpy, attrs, &config, 1, &count) || count != 1) fatal("EGL config");
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxattrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) fatal("EGL context");
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("GPU pipeline oracle v2 api=%s\nRenderer: %s\nGL: %s\n", api_name, renderer, glGetString(GL_VERSION));
    if (!software && (!renderer || !strstr(renderer, "radeonsi"))) {
        fputs("ERROR: renderer is not RadeonSI; hardware checks did not run\n", stderr); return 2;
    }
    texture_case(0); texture_case(1);
    ubo_case(0, 0); ubo_case(1, 0); ubo_case(0, 1);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx); eglTerminate(dpy);
    if (gbm) gbm_device_destroy(gbm);
    if (fd >= 0) close(fd);
    printf("SUMMARY failures=%d inconclusive_overlap_cases=%d mode=%s\n", failures, inconclusive,
           software ? "software-harness-only" : "RadeonSI-before-display");
    return failures ? 1 : (!software && inconclusive ? 2 : 0);
}
