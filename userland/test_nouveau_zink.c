#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

static void fail(const char *step) {
    fprintf(stderr, "NOUVEAU_C7: FAIL %s EGL=0x%x GL=0x%x\n", step, eglGetError(), glGetError());
    exit(1);
}

static void identity(int software) {
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Avory C7", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application};
    VkInstance instance;
    VkResult result = vkCreateInstance(&info, NULL, &instance);
    printf("NOUVEAU_C7: vkCreateInstance result=%d\n", result);
    if (result != VK_SUCCESS) fail("Vulkan instance");
    uint32_t count = 16;
    VkPhysicalDevice devices[16];
    result = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (result != VK_SUCCESS || !count || count > 16) fail("Vulkan enumeration");
    unsigned matches = 0;
    for (uint32_t index = 0; index < count; index++) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[index], &properties);
        printf("NOUVEAU_C7: Vulkan vendor=%04x device=%04x type=%u name=%s\n",
               properties.vendorID, properties.deviceID, properties.deviceType, properties.deviceName);
        if (software ? properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU :
            properties.vendorID == 0x10de && properties.deviceID == 0x2803 &&
            properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            strstr(properties.deviceName, "NVK")) matches++;
    }
    if (matches != 1) fail("Vulkan exact device identity");
    vkDestroyInstance(instance, NULL);
    puts("NOUVEAU_C7: PASS Vulkan device identity and instance teardown");
}

static GLuint shader(GLenum type, const char *source) {
    GLuint object = glCreateShader(type);
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint status;
    glGetShaderiv(object, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[4096];
        glGetShaderInfoLog(object, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
        fail("shader compile");
    }
    return object;
}

static void pixels(unsigned char *output, unsigned seed) {
    for (unsigned row = 0; row < 64; row++)
        for (unsigned column = 0; column < 64; column++) {
            unsigned offset = (row * 64 + column) * 4;
            output[offset] = (column * 13 + row * 7 + seed * 31) & 255;
            output[offset + 1] = (column * 3 + row * 11 + seed * 47) & 255;
            output[offset + 2] = (column * 19 + row * 5 + seed * 61) & 255;
            output[offset + 3] = 255;
        }
}

static void draw_current(int software, unsigned cycle) {
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("NOUVEAU_C7: OpenGL cycle=%u renderer=%s version=%s\n", cycle,
           renderer ? renderer : "missing", glGetString(GL_VERSION));
    if (!renderer || !strstr(renderer, "zink") ||
        (software ? !strstr(renderer, "llvmpipe") :
         !strstr(renderer, "NVK") || !strstr(renderer, "4060") || strstr(renderer, "llvmpipe")))
        fail("Zink renderer identity");
    GLuint vertex = shader(GL_VERTEX_SHADER,
        "#version 330 core\n"
        "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}");
    GLuint fragment = shader(GL_FRAGMENT_SHADER,
        "#version 330 core\n"
        "uniform uint seed;out vec4 color;void main(){uvec2 p=uvec2(gl_FragCoord.xy);"
        "uvec3 v=uvec3(p.x*13u+p.y*7u+seed*31u,p.x*3u+p.y*11u+seed*47u,"
        "p.x*19u+p.y*5u+seed*61u);color=vec4(vec3(v&uvec3(255u))/255.0,1.0);}");
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) fail("program link");
    GLuint texture, framebuffer, array;
    glGenVertexArrays(1, &array); glBindVertexArray(array);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fail("framebuffer");
    glViewport(0, 0, 64, 64); glDisable(GL_DITHER); glUseProgram(program);
    for (unsigned frame = 0; frame < 4; frame++) {
        unsigned seed = cycle * 4 + frame;
        unsigned char expected[64 * 64 * 4], actual[64 * 64 * 4];
        memset(actual, 0xa5, sizeof(actual)); pixels(expected, seed);
        glUniform1ui(glGetUniformLocation(program, "seed"), seed);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        GLenum waited = glClientWaitSync(fence, 0, 10000000000ull);
        if (waited != GL_ALREADY_SIGNALED && waited != GL_CONDITION_SATISFIED) fail("bounded GPU fence");
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, actual);
        if (glGetError() != GL_NO_ERROR || memcmp(actual, expected, sizeof(actual))) fail("exact shader pixels");
        glDeleteSync(fence);
        printf("NOUVEAU_C7: PASS pixels cycle=%u frame=%u pixels=4096\n", cycle, frame);
    }
    glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture);
    glDeleteVertexArrays(1, &array); glDeleteProgram(program);
    glDeleteShader(vertex); glDeleteShader(fragment);
    if (glGetError() != GL_NO_ERROR) fail("GL cleanup");
}

static void render(int software, unsigned cycle) {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major, minor, count;
    if (!eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API)) fail("EGL initialization");
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig configuration;
    if (!eglChooseConfig(display, attributes, &configuration, 1, &count) || count != 1) fail("EGL config");
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, configuration, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        fail("EGL context");
    draw_current(software, cycle);
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ||
        !eglDestroyContext(display, context) || !eglTerminate(display)) fail("EGL cleanup");
    printf("NOUVEAU_C7: PASS context teardown cycle=%u\n", cycle);
}

int main(int argc, char **argv) {
    int software = argc == 2 && !strcmp(argv[1], "--software");
    if (argc != 1 && !software) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setenv("VK_DRIVER_FILES", software ? "/usr/share/vulkan/icd.d/lvp_icd.json" :
           "/usr/share/vulkan/icd.d/nouveau_icd.json", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
    setenv("GALLIUM_DRIVER", "zink", 1);
    setenv("EGL_PLATFORM", "surfaceless", 1);
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    if (software) setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    else unsetenv("LIBGL_ALWAYS_SOFTWARE");
    setenv("EGL_LOG_LEVEL", "debug", 1);
    printf("NOUVEAU_C7: START mode=%s\n", software ? "software" : "hardware");
    identity(software);
    for (unsigned cycle = 0; cycle < 3; cycle++) render(software, cycle);
    puts("NOUVEAU_C7: END shader pixels and context restart status=pass");
    return 0;
}
