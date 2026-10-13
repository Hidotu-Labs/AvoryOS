/* Offscreen shader-constant churn with overlapping IBs. Readback happens
 * after a complete batch, rather than synchronizing after every draw.
 * --software is a harness smoke test; the default requires real RadeonSI. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

static int egl_failure(const char *step) {
    fprintf(stderr, "FAIL: %s (EGL error 0x%x); rendering checks did not run\n",
            step, eglGetError());
    return 2;
}

static GLuint shader(GLenum type, const char *source) {
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &source, NULL); glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "%s\n", log); exit(1);
    }
    return s;
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

static void color(unsigned draw, unsigned batch, unsigned char rgba[4]) {
    rgba[0] = (draw * 37 + batch * 53) & 255;
    rgba[1] = (draw * 71 + batch * 29) & 255;
    rgba[2] = (draw * 17 + batch * 97) & 255;
    rgba[3] = 255;
}

int main(int argc, char **argv) {
    enum { GRID = 32, TILE = 64, SIDE = GRID * TILE, BATCHES = 16 };
    int software = argc == 2 && !strcmp(argv[1], "--software");
    if (argc != 1 && !software) {
        fprintf(stderr, "Usage: %s [--software]\n", argv[0]); return 2;
    }
    int fd = software ? -1 : amd_node();
    if (!software && fd < 0) {
        fputs("No AMD render node; hardware test was not run.\n", stderr); return 2;
    }
    struct gbm_device *gbm = software ? NULL : gbm_create_device(fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!platform || (!software && !gbm)) return egl_failure("GBM/platform display setup");
    if (software) setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    EGLDisplay dpy = platform(software ? EGL_PLATFORM_SURFACELESS_MESA : EGL_PLATFORM_GBM_KHR,
                             gbm, NULL);
    EGLint major, minor, count;
    if (!eglInitialize(dpy, &major, &minor)) return egl_failure("eglInitialize");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return egl_failure("eglBindAPI");
    EGLint attrs[] = {EGL_SURFACE_TYPE, software ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config;
    if (!eglChooseConfig(dpy, attrs, &config, 1, &count) || count != 1)
        return egl_failure("eglChooseConfig");
    EGLint ctxattrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxattrs);
    if (ctx == EGL_NO_CONTEXT) return egl_failure("eglCreateContext");
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return egl_failure("eglMakeCurrent");
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("Renderer: %s\nGL: %s\n", renderer, glGetString(GL_VERSION));
    if (!software && (!renderer || !strstr(renderer, "radeonsi"))) {
        fputs("FAIL: renderer is not RadeonSI; hardware checks did not run\n", stderr);
        return 2;
    }
    GLuint vs = shader(GL_VERTEX_SHADER,
        "attribute vec2 pos; void main(){gl_Position=vec4(pos,0.0,1.0);}");
    GLuint fs = shader(GL_FRAGMENT_SHADER,
        "precision highp float; uniform vec4 uploaded; void main(){gl_FragColor=uploaded;}");
    GLuint program = glCreateProgram();
    glAttachShader(program, vs); glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "pos"); glLinkProgram(program);
    GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048]; glGetProgramInfoLog(program, sizeof log, NULL, log);
        fprintf(stderr, "FAIL: shader link: %s\n", log); return 1;
    }
    glUseProgram(program);
    GLint uniform = glGetUniformLocation(program, "uploaded");
    assert(uniform >= 0);
    GLuint vbo, texture, fbo;
    const GLfloat quad[] = {-1,-1, 1,-1, -1,1, 1,1};
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIDE, SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FAIL: framebuffer setup 0x%x\n", framebuffer_status); return 1;
    }
    glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    unsigned char *pixels = malloc((size_t)SIDE * SIDE * 4);
    if (!pixels) { perror("FAIL: readback allocation"); return 2; }
    for (unsigned batch = 0; batch < BATCHES; batch++) {
        for (unsigned draw = 0; draw < GRID * GRID; draw++) {
            unsigned char c[4]; color(draw, batch, c);
            glViewport((draw % GRID) * TILE, (draw / GRID) * TILE, TILE, TILE);
            glUniform4f(uniform, c[0]/255.0f, c[1]/255.0f, c[2]/255.0f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFlush(); /* Submit another IB without a CPU completion wait. */
        }
        glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        GLenum error = glGetError();
        if (error) { fprintf(stderr, "GL error: 0x%x\n", error); return 1; }
        for (unsigned y = 0; y < SIDE; y++) for (unsigned x = 0; x < SIDE; x++) {
            unsigned char expected[4]; color((y/TILE)*GRID + x/TILE, batch, expected);
            unsigned char *actual = pixels + ((size_t)y * SIDE + x) * 4;
            for (unsigned k = 0; k < 4; k++) if (abs((int)actual[k] - expected[k]) > 1) {
                fprintf(stderr, "FAIL batch=%u tile=%u pixel=(%u,%u) channel=%u expected=%u actual=%u\n",
                        batch, (y/TILE)*GRID+x/TILE, x, y, k, expected[k], actual[k]); return 1;
            }
        }
        printf("PASS batch %u: 1024 asynchronous constant uploads, all pixels match\n", batch);
        fflush(stdout);
    }
    free(pixels);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &texture); glDeleteBuffers(1, &vbo);
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx); eglTerminate(dpy);
    if (gbm) gbm_device_destroy(gbm);
    if (fd >= 0) close(fd);
    puts(software ? "PASS: software harness only; RadeonSI hardware remains untested" :
                    "PASS: RadeonSI asynchronous constants before display/PRIME");
    return 0;
}
