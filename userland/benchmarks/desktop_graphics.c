/* GLX compatibility-context workload; libraries are optional at runtime. */
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES 1
#include "desktop.h"
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XL(name)                                                               \
  __typeof__(name) *p_##name = dlsym(x, #name);                                \
  if (!p_##name)                                                               \
  goto missing
#define GL(name)                                                               \
  __typeof__(name) *p_##name = dlsym(g, #name);                                \
  if (!p_##name)                                                               \
  goto missing
int bench_graphics(unsigned frames) {
  if (!getenv("DISPLAY")) {
    bench_skip("graphics", "DISPLAY_unset");
    return 0;
  }
  void *x = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL),
       *g = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!x || !g) {
    bench_skip("graphics", "X11_or_OpenGL_library_unavailable");
    goto close_libs;
  }
  XL(XOpenDisplay);
  XL(XCloseDisplay);
  XL(XCreateColormap);
  XL(XCreateWindow);
  XL(XDestroyWindow);
  XL(XFreeColormap);
  XL(XFree);
  XL(XMapWindow);
  XL(XSync);
  XL(XStoreName);
  XL(XInternAtom);
  XL(XGetSelectionOwner);
  XL(XPending);
  XL(XNextEvent);
  XL(XSendEvent);
  XL(XFlush);
  GL(glXChooseVisual);
  GL(glXCreateContext);
  GL(glXMakeCurrent);
  GL(glXDestroyContext);
  GL(glXSwapBuffers);
  GL(glXGetProcAddressARB);
  GL(glXQueryExtensionsString);
  GL(glXIsDirect);
  GL(glGetString);
  GL(glGetError);
  GL(glViewport);
  GL(glClearColor);
  GL(glClear);
  GL(glBegin);
  GL(glEnd);
  GL(glColor4f);
  GL(glVertex2f);
  GL(glFinish);
  GL(glEnable);
  GL(glDisable);
  GL(glBlendFunc);
  GL(glGenTextures);
  GL(glBindTexture);
  GL(glTexImage2D);
  GL(glTexParameteri);
  GL(glTexCoord2f);
  GL(glDeleteTextures);
  GL(glReadPixels);
  Display *display = p_XOpenDisplay(NULL);
  if (!display) {
    bench_skip("graphics", "cannot_open_X_display");
    goto close_libs;
  }
  int screen = DefaultScreen(display),
      attr[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None};
  XVisualInfo *visual = p_glXChooseVisual(display, screen, attr);
  if (!visual) {
    bench_skip("graphics", "no_double_buffered_GLX_visual");
    p_XCloseDisplay(display);
    goto close_libs;
  }
  Colormap cmap = p_XCreateColormap(
      display, RootWindow(display, visual->screen), visual->visual, AllocNone);
  XSetWindowAttributes attrs = {.colormap = cmap,
                                .event_mask = StructureNotifyMask};
  Window window =
      p_XCreateWindow(display, RootWindow(display, visual->screen), 0, 0, 800,
                      600, 0, visual->depth, InputOutput, visual->visual,
                      CWColormap | CWEventMask, &attrs);
  GLXContext context = p_glXCreateContext(display, visual, NULL, True);
  if (!context || !p_glXMakeCurrent(display, window, context)) {
    bench_skip("graphics", "GLX_context_unavailable");
    goto destroy;
  }
  p_XStoreName(display, window, "Avory desktop latency benchmark (800x600)");
  p_XMapWindow(display, window);
  p_XSync(display, False);
  const char *renderer = (const char *)p_glGetString(GL_RENDERER),
             *version = (const char *)p_glGetString(GL_VERSION);
  char atom[64];
  snprintf(atom, sizeof(atom), "_NET_WM_CM_S%d", screen);
  int compositor = p_XGetSelectionOwner(
                       display, p_XInternAtom(display, atom, False)) != None;
  const char *require_compositor = getenv("AVORY_BENCH_REQUIRE_COMPOSITOR");
  if (require_compositor && !strcmp(require_compositor, "1")) {
    uint64_t deadline = bench_now() + 60000000000ULL;
    while (!compositor && bench_now() < deadline) {
      struct timespec delay = {0, 10000000};
      nanosleep(&delay, NULL);
      compositor = p_XGetSelectionOwner(
          display, p_XInternAtom(display, atom, False)) != None;
    }
    if (!compositor) {
      errno = ETIMEDOUT;
      bench_fail("compositor_ready");
      p_glXMakeCurrent(display, None, NULL);
      goto destroy;
    }
  }
  puts("GRAPHICS protocol=separate_queries_v2");
  printf("GRAPHICS renderer=%s version=%s direct=%d compositor=%s width=800 "
         "height=600 layers=12 triangles=256\n",
         renderer ? renderer : "unknown", version ? version : "unknown",
         p_glXIsDirect(display, context), compositor ? "active" : "absent");
  if (renderer &&
      (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe") ||
       strstr(renderer, "Software")))
    puts("GRAPHICS execution=software_renderer");
  else
    puts("GRAPHICS "
         "execution=driver_reported_renderer_verify_hardware_in_run_metadata");
  if (!compositor)
    bench_skip("desktop_compositor", "no_X_compositing_manager_selection");
  /* Require both extension advertisement and function pointer. GLX often
   * returns a trampoline even for unsupported entry points. */
  const char *extensions = p_glXQueryExtensionsString(display, screen);
  void (*swap_interval)(Display *, GLXDrawable, int) =
      (void *)p_glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
  int vsync =
      extensions && strstr(extensions, "GLX_EXT_swap_control") && swap_interval;
  if (vsync)
    swap_interval(display, window, 1);
  else
    bench_skip("vsync_control", "GLX_EXT_swap_control_unavailable");
  Bool (*sync_values)(Display *, GLXDrawable, int64_t *, int64_t *, int64_t *) =
      (void *)p_glXGetProcAddressARB((const GLubyte *)"glXGetSyncValuesOML");
  Bool (*wait_sbc)(Display *, GLXDrawable, int64_t, int64_t *, int64_t *,
                   int64_t *) =
      (void *)p_glXGetProcAddressARB((const GLubyte *)"glXWaitForSbcOML");
  int oml = extensions && strstr(extensions, "GLX_OML_sync_control") &&
            sync_values && wait_sbc;
  void (*gen_queries)(GLsizei, GLuint *) =
      (void *)p_glXGetProcAddressARB((const GLubyte *)"glGenQueries");
  void (*begin_query)(GLenum, GLuint) =
      (void *)p_glXGetProcAddressARB((const GLubyte *)"glBeginQuery");
  void (*end_query)(GLenum) =
      (void *)p_glXGetProcAddressARB((const GLubyte *)"glEndQuery");
  void (*query_result)(GLuint, GLenum, GLuint64 *) =
      (void *)p_glXGetProcAddressARB((const GLubyte *)"glGetQueryObjectui64v");
  void (*delete_queries)(GLsizei, const GLuint *) =
      (void *)p_glXGetProcAddressARB((const GLubyte *)"glDeleteQueries");
  const char *gl_ext = (const char *)p_glGetString(GL_EXTENSIONS);
  int major = 0, minor = 0;
  if (version)
    sscanf(version, "%d.%d", &major, &minor);
  int gpu_timer = ((major > 3 || (major == 3 && minor >= 3)) ||
                   (gl_ext && strstr(gl_ext, "GL_ARB_timer_query"))) &&
                  gen_queries && begin_query && end_query && query_result &&
                  delete_queries;
  const char *timer_setting = getenv("AVORY_BENCH_GPU_TIMER");
  if (timer_setting && !strcmp(timer_setting, "0"))
    gpu_timer = 0;
  printf("GRAPHICS gpu_timer=%s\n", gpu_timer ? "enabled" : "disabled_or_unavailable");
  uint64_t *submit = calloc(frames, sizeof(*submit)),
           *finish = calloc(frames, sizeof(*finish));
  uint64_t *query_finish = calloc(frames, sizeof(*query_finish)),
           *query_readback = calloc(frames, sizeof(*query_readback));
  uint64_t *swaps = calloc(frames, sizeof(*swaps)),
           *intervals = calloc(frames, sizeof(*intervals));
  uint64_t *present = calloc(frames, sizeof(*present)),
           *gpu = calloc(frames, sizeof(*gpu));
  uint64_t *delivery = calloc(frames, sizeof(*delivery)),
           *response = calloc(frames, sizeof(*response));
  if (!query_finish || !query_readback || !submit || !finish || !swaps || !intervals || !present || !gpu ||
      !delivery || !response) {
    bench_fail("graphics_samples");
    goto free_samples;
  }
  GLuint texture = 0, query = 0;
  p_glGenTextures(1, &texture);
  p_glBindTexture(GL_TEXTURE_2D, texture);
  unsigned char pixels[128 * 128 * 4];
  for (unsigned i = 0; i < sizeof(pixels); i++)
    pixels[i] = (i % 4 == 3) ? 160 : (unsigned char)(i * 17);
  p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
  if (gpu_timer)
    gen_queries(1, &query);
  p_glViewport(0, 0, 800, 600);
  p_glClearColor(.05f, .1f, .15f, 1);
  p_glDisable(GL_DEPTH_TEST);
  unsigned present_count = 0, missed_refreshes = 0;
  uint64_t previous_wall = 0;
  int64_t previous_ust = 0, previous_msc = 0, previous_sbc = 0;
  Atom action = p_XInternAtom(display, "_AVORY_BENCH_ACTION", False);
  unsigned invalid_gpu_samples = 0;
  /* Queries can force a renderer readback/poll before a fence is retired.
   * Measure plain completion, instrumented GPU timing and queued presentation
   * in separate phases so query handling cannot contaminate glFinish latency. */
  for (unsigned phase = 0; phase < 3; phase++)
    for (unsigned i = 0; i < frames + 10; i++) {
      if (phase == 1 && !gpu_timer)
        continue;
      while (p_XPending(display)) {
        XEvent event;
        p_XNextEvent(display, &event);
      }
      uint64_t input_start = bench_now(), delivered = input_start;
      if (phase == 2) {
        XEvent event = {0};
        event.xclient.type = ClientMessage;
        event.xclient.display = display;
        event.xclient.window = window;
        event.xclient.message_type = action;
        event.xclient.format = 32;
        event.xclient.data.l[0] = i;
        p_XSendEvent(display, window, False, NoEventMask, &event);
        p_XFlush(display);
        do {
          p_XNextEvent(display, &event);
        } while (event.type != ClientMessage ||
                 event.xclient.message_type != action ||
                 event.xclient.data.l[0] != (long)i);
        delivered = bench_now();
      }
      uint64_t start = bench_now();
      if (phase == 1)
        begin_query(GL_TIME_ELAPSED, query);
      p_glClear(GL_COLOR_BUFFER_BIT);
      p_glDisable(GL_TEXTURE_2D);
      p_glDisable(GL_BLEND);
      p_glBegin(GL_TRIANGLES);
      for (unsigned t = 0; t < 256; t++) {
        float a = -.95f + (t % 16) * .12f, b = -.95f + (t / 16) * .12f;
        p_glColor4f((t % 7) / 7.f, .5f, .8f, 1);
        p_glVertex2f(a, b);
        p_glVertex2f(a + .1f, b);
        p_glVertex2f(a, b + .1f);
      }
      p_glEnd();
      p_glEnable(GL_TEXTURE_2D);
      p_glEnable(GL_BLEND);
      p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      for (unsigned layer = 0; layer < 12; layer++) {
        float a = -.95f + layer * .04f, b = .95f - layer * .025f;
        p_glColor4f(1, 1, 1, .35f);
        p_glBegin(GL_QUADS);
        p_glTexCoord2f(0, 0);
        p_glVertex2f(a, a);
        p_glTexCoord2f(1, 0);
        p_glVertex2f(b, a);
        p_glTexCoord2f(1, 1);
        p_glVertex2f(b, b);
        p_glTexCoord2f(0, 1);
        p_glVertex2f(a, b);
        p_glEnd();
      }
      if (phase < 2) {
        if (phase == 1)
          end_query(GL_TIME_ELAPSED);
        uint64_t submitted = bench_now();
        p_glFinish();
        uint64_t completed = bench_now();
        if (i >= 10) {
          if (phase == 0) {
            submit[i - 10] = submitted - start;
            finish[i - 10] = completed - submitted;
          } else {
            GLuint64 ns = 0;
            query_result(query, GL_QUERY_RESULT, &ns);
            uint64_t read_back = bench_now();
            query_finish[i - 10] = completed - submitted;
            query_readback[i - 10] = read_back - completed;
            gpu[i - 10] = ns;
            /* Allow clock/query resolution error, but reject device values
             * exceeding the complete enclosing wall interval by >1 ms. */
            if (ns > read_back - start + 1000000ULL)
              invalid_gpu_samples++;
          }
        }
      } else {
        int64_t ust = 0, msc = 0, sbc = 0, target = 0;
        if (oml) {
          if (!sync_values(display, window, &ust, &msc, &sbc))
            oml = 0;
          else
            target = sbc + 1;
        }
        uint64_t swap_start = bench_now();
        p_glXSwapBuffers(display, window);
        uint64_t swapped = bench_now();
        if (oml && !wait_sbc(display, window, target, &ust, &msc, &sbc))
          oml = 0;
        uint64_t end = bench_now();
        if (i >= 10) {
          swaps[i - 10] = swapped - swap_start;
          intervals[i - 10] = end - previous_wall;
          delivery[i - 10] = delivered - input_start;
          response[i - 10] = end - input_start;
          if (oml && previous_ust > 0 && ust > previous_ust &&
              sbc > previous_sbc) {
            present[present_count++] = (uint64_t)(ust - previous_ust) * 1000;
            if (msc > previous_msc + 1)
              missed_refreshes += (unsigned)(msc - previous_msc - 1);
          }
        }
        previous_wall = end;
        previous_ust = ust;
        previous_msc = msc;
        previous_sbc = sbc;
      }
    }
  unsigned char pixel[4];
  p_glReadPixels(400, 300, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  GLenum error = p_glGetError();
  if (error) {
    printf("GRAPHICS gl_error=0x%x\n", error);
    errno = EIO;
    bench_fail("graphics_render");
  } else {
    printf("GRAPHICS pixel=%u,%u,%u,%u vsync_requested=%d oml_available=%d\n",
           pixel[0], pixel[1], pixel[2], pixel[3], vsync, oml);
    if (present_count)
      printf("GRAPHICS OML_valid_samples=%u missed_refreshes=%u\n",
             present_count, missed_refreshes);
    else
      puts("GRAPHICS missed_refreshes=unavailable");
    bench_stats("gl_scene_and_12layer_blend_submit", submit, frames);
    bench_stats("gl_finish_completion_wait", finish, frames);
    if (gpu_timer) {
      bench_stats("gl_query_instrumented_finish_wait", query_finish, frames);
      bench_stats("gl_query_result_readback", query_readback, frames);
      if (!invalid_gpu_samples)
        bench_stats("gl_scene_and_blend_GPU_timer", gpu, frames);
      else {
        printf("GRAPHICS invalid_gpu_timer_samples=%u total=%u\n",
               invalid_gpu_samples, frames);
        bench_skip("gpu_device_timer", "device_time_exceeds_enclosing_wall_interval");
      }
    } else
      bench_skip("gpu_device_timer", timer_setting && !strcmp(timer_setting, "0")
                    ? "disabled_by_configuration" : "ARB_timer_query_unavailable");
    bench_stats("gl_swap_call", swaps, frames);
    bench_stats("gl_frame_completion_interval_wall", intervals, frames);
    bench_stats("x11_synthetic_action_delivery", delivery, frames);
    bench_stats("x11_synthetic_action_to_swap_completion_proxy", response,
                frames);
    if (present_count)
      bench_stats("gl_OML_swap_completion_interval", present, present_count);
    else
      bench_skip("presentation_timestamps",
                 "OML_completion_unavailable_wall_intervals_are_only_a_proxy");
    puts("GRAPHICS measurement=synthetic_layer_composition_and_window_swaps "
         "physical_input_to_photon=not_measured");
  }
  if (query)
    delete_queries(1, &query);
  p_glDeleteTextures(1, &texture);
free_samples:
  free(submit);
  free(finish);
  free(query_finish);
  free(query_readback);
  free(swaps);
  free(intervals);
  free(present);
  free(gpu);
  free(delivery);
  free(response);
  p_glXMakeCurrent(display, None, NULL);
destroy:
  if (context)
    p_glXDestroyContext(display, context);
  p_XDestroyWindow(display, window);
  p_XFreeColormap(display, cmap);
  p_XFree(visual);
  p_XCloseDisplay(display);
  goto close_libs;
missing:
  bench_skip("graphics", "required_X11_or_GL_symbol_unavailable");
close_libs:
  if (g)
    dlclose(g);
  if (x)
    dlclose(x);
  return 0;
}
