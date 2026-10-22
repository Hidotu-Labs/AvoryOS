#define main c7_headless_main
#include "test_nouveau_zink.c"
#undef main
#include <signal.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>
#include <X11/Xlib-xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

static pid_t server = -1;

static void stop_server(void) {
    if (server > 0) {
        kill(server, SIGTERM);
        int status = 0, stopped = 0;
        for (unsigned attempt = 0; attempt < 100; attempt++) {
            if (waitpid(server, &status, WNOHANG) == server) { stopped = 1; break; }
            usleep(100000);
        }
        if (!stopped) { kill(server, SIGKILL); waitpid(server, &status, 0); }
        printf("NOUVEAU_C7_XORG: STOP bounded=%d status=%d\n", stopped, status);
        server = -1;
    }
    FILE *log = fopen("/tmp/Xorg.log", "r");
    if (log) {
        char line[2048];
        while (fgets(line, sizeof(line), log)) fputs(line, stdout);
        fclose(log);
    }
}

static void rectangle(int left, int bottom, int width, int height, unsigned color) {
    glScissor(left, bottom, width, height);
    glClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                 (color & 255) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

int main(int argc, char **argv) {
    int output = open("/dev/kmsg", O_WRONLY);
    if (output >= 0) { dup2(output, 1); dup2(output, 2); close(output); }
    int result = c7_headless_main(argc, argv);
    if (result) return result;
    if (argc == 2) {
        puts("NOUVEAU_C7_XORG: SOFTWARE profile compiled; no hardware Xorg claim");
        return 0;
    }
    if (system("/bin/sh /usr/bin/avory-xorg-profile /sys /etc/X11/xorg.conf /usr/share/avory-xorg"))
        fail("NVIDIA Xorg identity selection");
    atexit(stop_server);
    unsetenv("GALLIUM_DRIVER"); unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
    unsetenv("VK_DRIVER_FILES"); unsetenv("EGL_PLATFORM");
    setenv("NOUVEAU_USE_ZINK", "1", 1);
    server = fork();
    if (server < 0) fail("Xorg fork");
    if (!server) {
        unsetenv("GALLIUM_DRIVER"); unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
        unsetenv("VK_DRIVER_FILES"); unsetenv("EGL_PLATFORM");
        execl("/usr/lib/Xorg", "Xorg", ":0", "-ac", "-nolisten", "tcp", "-noreset",
              "-config", "/etc/X11/xorg.conf", "-logfile", "/tmp/Xorg.log", NULL);
        _exit(127);
    }
    Display *display = NULL;
    for (unsigned attempt = 0; attempt < 150; attempt++) {
        display = XOpenDisplay(":0");
        if (display) break;
        usleep(100000);
    }
    if (!display) fail("Xorg display startup");
    xcb_connection_t *connection = XGetXCBConnection(display);
    xcb_dri3_query_version_reply_t *dri = xcb_dri3_query_version_reply(connection,
        xcb_dri3_query_version(connection, 1, 2), NULL);
    xcb_present_query_version_reply_t *present = xcb_present_query_version_reply(connection,
        xcb_present_query_version(connection, 1, 2), NULL);
    printf("NOUVEAU_C7_XORG: protocols DRI3=%u.%u Present=%u.%u\n",
        dri ? dri->major_version : 0, dri ? dri->minor_version : 0,
        present ? present->major_version : 0, present ? present->minor_version : 0);
    free(dri); free(present);
    setenv("LIBGL_KOPPER_DRI2", "true", 1);
    setenv("DISPLAY", ":0", 1);
    int status = system("/usr/bin/avory-prime-display");
    if (status) fail("Xorg PRIME output binding");
    pid_t cube = fork();
    if (cube < 0) fail("vkcube fork");
    if (!cube) {
        setenv("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/nouveau_icd.json", 1);
        execl("/usr/bin/vkcube", "vkcube", "--wsi", "xcb", "--c", "600",
              "--width", "640", "--height", "480", NULL);
        _exit(127);
    }
    int cube_status = 0, cube_finished = 0;
    for (unsigned attempt = 0; attempt < 900; attempt++) {
        pid_t waited = waitpid(cube, &cube_status, WNOHANG);
        if (waited == cube) { cube_finished = 1; break; }
        if (waited < 0) fail("vkcube wait");
        usleep(100000);
    }
    if (!cube_finished) {
        kill(cube, SIGKILL); waitpid(cube, &cube_status, 0);
        fail("vkcube completion timeout");
    }
    if (!WIFEXITED(cube_status) || WEXITSTATUS(cube_status)) fail("vkcube exit");
    puts("NOUVEAU_C7_XORG: VKCUBE frames=600 status=pass");
    GLint attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                          GLX_BLUE_SIZE, 8, None};
    XVisualInfo *visual = glXChooseVisual(display, DefaultScreen(display), attributes);
    if (!visual) fail("GLX visual");
    XSetWindowAttributes settings = {.colormap = XCreateColormap(display,
        RootWindow(display, visual->screen), visual->visual, AllocNone), .override_redirect = True};
    Window window = XCreateWindow(display, RootWindow(display, visual->screen), 0, 0, 1280, 800,
        0, visual->depth, InputOutput, visual->visual, CWColormap | CWOverrideRedirect, &settings);
    XMapRaised(display, window); XSync(display, False);
    GLXContext context = glXCreateContext(display, visual, NULL, True);
    if (!context || !glXMakeCurrent(display, window, context)) fail("GLX context");
    draw_current(0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, 1280, 800); glEnable(GL_SCISSOR_TEST);
    static const unsigned colors[] = {0xff0000,0x00ff00,0x0000ff,0xffff00,0xff00ff,0x00ffff,0xffffff,0x202020};
    GC graphics = XCreateGC(display, window, 0, NULL);
    if (!graphics) fail("Xorg stress GC allocation");
    for (unsigned update = 0; update < 256; update++) {
        rectangle(0, 0, 1280, 800, colors[update % 8]);
        glXSwapBuffers(display, window); glFinish();
        if (glGetError() != GL_NO_ERROR) fail("GLX repeated update");
        Pixmap pixmap = XCreatePixmap(display, window, 1280, 800, visual->depth);
        unsigned long expected = update & 1 ? WhitePixel(display, visual->screen) :
                                             BlackPixel(display, visual->screen);
        XSetForeground(display, graphics, expected);
        XFillRectangle(display, pixmap, graphics, 0, 0, 1280, 800);
        XCopyArea(display, pixmap, window, graphics, 0, 0, 1280, 800, 0, 0);
        XImage *image = XGetImage(display, window, 640, 400, 1, 1, AllPlanes, ZPixmap);
        unsigned long mask = visual->red_mask | visual->green_mask | visual->blue_mask;
        if (!image || (XGetPixel(image, 0, 0) & mask) != (expected & mask))
            fail("Xorg repeated copy pixels");
        XDestroyImage(image);
        XFreePixmap(display, pixmap);
        if (update % 32 == 31)
            printf("NOUVEAU_C7_XORG: STRESS updates=%u pixels=pass\n", update + 1);
        usleep(20000);
    }
    XFreeGC(display, graphics);
    for (unsigned frame = 0; frame < 2; frame++) {
        for (unsigned bar = 0; bar < 8; bar++) rectangle(bar * 160, 0, 160, 800, colors[(bar + frame) % 8]);
        rectangle(8, 736, 120, 56, frame ? 0x00ffff : 0xffff00);
        rectangle(0, 0, 1280, 8, 0xffffff); rectangle(0, 792, 1280, 8, 0xffffff);
        rectangle(0, 0, 8, 800, 0xffffff); rectangle(1272, 0, 8, 800, 0xffffff);
        glXSwapBuffers(display, window); glFinish();
        if (glGetError() != GL_NO_ERROR) fail("GLX frame publication");
        printf("NOUVEAU_C7_XORG: READY frame=%u width=1280 height=800\n", frame);
        sleep(10);
    }
    glXMakeCurrent(display, None, NULL); glXDestroyContext(display, context);
    XDestroyWindow(display, window); XFreeColormap(display, settings.colormap);
    XFree(visual); XCloseDisplay(display);
    stop_server();
    puts("NOUVEAU_C7_XORG: END GLX shader pixels and PRIME presentation status=pass");
    return 0;
}
