/* Report the last GLX operation reached before a drawable stalls. */
#include <GL/glx.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *message) {
  fprintf(stderr, "%s\n", message);
  exit(1);
}

static int x_error(Display *display, XErrorEvent *error) {
  char message[128];
  XGetErrorText(display, error->error_code, message, sizeof(message));
  fprintf(stderr, "X11 error %s (request %u.%u)\n", message,
          error->request_code, error->minor_code);
  exit(1);
}

int main(int argc, char **argv) {
  const int size = 128;
  if (argc != 2 || (strcmp(argv[1], "window") && strcmp(argv[1], "pbuffer"))) {
    fprintf(stderr, "Usage: %s window|pbuffer\n", argv[0]);
    return 2;
  }
  int window_test = !strcmp(argv[1], "window");
  setvbuf(stdout, NULL, _IOLBF, 0);
  puts("Opening X display");
  Display *display = XOpenDisplay(NULL);
  if (!display)
    fail("XOpenDisplay failed");
  XSetErrorHandler(x_error);

  int screen = DefaultScreen(display);
  int count;
  const int attrs[] = {
      GLX_DRAWABLE_TYPE, window_test ? GLX_WINDOW_BIT : GLX_PBUFFER_BIT,
      GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
      GLX_BLUE_SIZE, 8, GLX_DOUBLEBUFFER, window_test ? True : False, None,
  };
  puts("Selecting GLX framebuffer configuration");
  GLXFBConfig *configs = glXChooseFBConfig(display, screen, attrs, &count);
  if (!configs || !count)
    fail("No matching GLX framebuffer configuration");

  puts("Creating GLX context");
  GLXContext context =
      glXCreateNewContext(display, configs[0], GLX_RGBA_TYPE, NULL, True);
  if (!context)
    fail("glXCreateNewContext failed");

  Window window = None;
  Colormap colormap = None;
  GLXDrawable drawable;
  if (window_test) {
    puts("Creating and mapping X11 window");
    XVisualInfo *visual = glXGetVisualFromFBConfig(display, configs[0]);
    if (!visual)
      fail("No X11 visual for GLX configuration");
    Window root = RootWindow(display, screen);
    colormap = XCreateColormap(display, root, visual->visual, AllocNone);
    XSetWindowAttributes attributes = {
        .colormap = colormap, .event_mask = StructureNotifyMask,
    };
    window = XCreateWindow(display, root, 32, 32, size, size, 0, visual->depth,
                           InputOutput, visual->visual,
                           CWColormap | CWEventMask, &attributes);
    XFree(visual);
    XStoreName(display, window, "AvoryOS GLX window test");
    XMapWindow(display, window);
    XEvent event;
    XWindowEvent(display, window, StructureNotifyMask, &event);
    while (event.type != MapNotify)
      XWindowEvent(display, window, StructureNotifyMask, &event);
    drawable = glXCreateWindow(display, configs[0], window, NULL);
  } else {
    puts("Creating offscreen GLX drawable");
    const int dimensions[] = {GLX_PBUFFER_WIDTH, size, GLX_PBUFFER_HEIGHT, size,
                              None};
    drawable = glXCreatePbuffer(display, configs[0], dimensions);
  }
  XFree(configs);
  if (!drawable)
    fail("Failed to create GLX drawable");

  puts("Binding GLX context");
  if (!glXMakeContextCurrent(display, drawable, drawable, context))
    fail("glXMakeContextCurrent failed");
  printf("Renderer: %s, direct: %s\n", glGetString(GL_RENDERER),
         glXIsDirect(display, context) ? "yes" : "no");
  glViewport(0, 0, size, size);
  for (int frame = 0; frame < 2; frame++) {
    printf("Frame %d: clearing and waiting for rendering\n", frame + 1);
    glClearColor(frame == 0, frame == 1, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    puts("Reading rendered pixel");
    unsigned char pixel[4];
    glReadPixels(size / 2, size / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("GL pixel: %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    if (glGetError() != GL_NO_ERROR || pixel[frame] < 250 ||
        pixel[1 - frame] > 5 || pixel[2] > 5)
      fail("Unexpected rendered pixel");
    if (window_test) {
      puts("Swapping window buffers");
      glXSwapBuffers(display, drawable);
      puts("Swap returned, synchronizing with Xorg");
      XSync(display, False);
      sleep(1);
    }
  }

  puts("Releasing drawable and context");
  glXMakeContextCurrent(display, None, None, NULL);
  if (window_test) {
    glXDestroyWindow(display, drawable);
    XDestroyWindow(display, window);
    XFreeColormap(display, colormap);
  } else {
    glXDestroyPbuffer(display, drawable);
  }
  glXDestroyContext(display, context);
  XCloseDisplay(display);
  puts("GLX stages passed");
  return 0;
}
