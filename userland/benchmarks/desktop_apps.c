#define _GNU_SOURCE
#include "desktop.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int load_math(void) {
  void *h = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    return -1;
  double (*cosine)(double) = dlsym(h, "cos");
  int ok = cosine && cosine(0.0) == 1.0;
  dlclose(h);
  return ok ? 0 : -1;
}
int bench_app_child(int fd) {
  if (load_math())
    return 2;
  uint64_t t = bench_now();
  return bench_transfer(fd, &t, sizeof(t), 1) ? 3 : 0;
}
static void process_tree(unsigned samples, const char *self) {
  uint64_t *ready = calloc(samples, sizeof(*ready)),
           *complete = calloc(samples, sizeof(*complete));
  if (!ready || !complete) {
    free(ready);
    free(complete);
    bench_fail("app_tree_alloc");
    return;
  }
  unsigned done = 0;
  for (; done < samples; done++) {
    int pipes[4][2];
    unsigned opened = 0;
    for (; opened < 4; opened++)
      if (pipe(pipes[opened]))
        break;
    if (opened < 4) {
      for (unsigned j = 0; j < opened; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
      }
      bench_fail("app_tree_pipe");
      break;
    }
    pid_t children[4];
    unsigned started = 0;
    uint64_t latest = 0, start = bench_now();
    fflush(NULL);
    for (; started < 4; started++) {
      pid_t p = fork();
      if (p < 0)
        break;
      if (!p) {
        for (unsigned j = 0; j < 4; j++) {
          close(pipes[j][0]);
          if (j != started)
            close(pipes[j][1]);
        }
        char fd[24];
        snprintf(fd, sizeof(fd), "%d", pipes[started][1]);
        execl(self, self, "--app-child", fd, (char *)NULL);
        _exit(127);
      }
      children[started] = p;
    }
    for (unsigned j = 0; j < 4; j++)
      close(pipes[j][1]);
    int bad = started != 4;
    /* Each child emits just eight bytes; wait before reading avoids a
     * read-to-EOF/reaper race and cannot fill the pipe. */
    for (unsigned j = 0; j < started; j++)
      if (bench_wait(children[j]))
        bad = 1;
    uint64_t ended = bench_now();
    for (unsigned j = 0; j < 4; j++) {
      uint64_t t = 0;
      if (j < started &&
          (bench_transfer(pipes[j][0], &t, sizeof(t), 0) || t < start)) {
        printf("APP worker=%u ready=%" PRIu64 " start=%" PRIu64 " errno=%d\n",
               j, t, start, errno);
        bad = 1;
      }
      if (t > latest)
        latest = t;
      close(pipes[j][0]);
    }
    if (bad) {
      errno = ECHILD;
      bench_fail("app_tree_worker");
      break;
    }
    ready[done] = latest - start;
    complete[done] = ended - start;
  }
  if (done == samples) {
    bench_stats("app_4process_exec_dlopen_ready", ready, samples);
    bench_stats("app_4process_exec_dlopen_reap", complete, samples);
  }
  free(ready);
  free(complete);
}
#define LOAD(handle, name)                                                     \
  __typeof__(name) *p_##name = dlsym(handle, #name);                           \
  if (!p_##name)                                                               \
  goto missing
static void fonts(unsigned samples) {
  void *fc = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
  void *ft = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!fc || !ft) {
    bench_skip("fonts", "fontconfig_or_freetype_unavailable");
    goto close_libs;
  }
  LOAD(fc, FcInitLoadConfigAndFonts);
  LOAD(fc, FcNameParse);
  LOAD(fc, FcConfigSubstitute);
  LOAD(fc, FcDefaultSubstitute);
  LOAD(fc, FcFontMatch);
  LOAD(fc, FcPatternGetString);
  LOAD(fc, FcPatternDestroy);
  LOAD(fc, FcConfigDestroy);
  LOAD(ft, FT_Init_FreeType);
  LOAD(ft, FT_New_Face);
  LOAD(ft, FT_Set_Pixel_Sizes);
  LOAD(ft, FT_Load_Char);
  LOAD(ft, FT_Done_Face);
  LOAD(ft, FT_Done_FreeType);
  uint64_t start = bench_now();
  FcConfig *config = p_FcInitLoadConfigAndFonts();
  uint64_t init = bench_now() - start;
  if (!config) {
    errno = EIO;
    bench_fail("fontconfig_init");
    goto close_libs;
  }
  bench_stats("fontconfig_init", &init, 1);
  start = bench_now();
  FcPattern *query = p_FcNameParse((FcChar8 *)"sans:size=18");
  FcPattern *match = NULL;
  FcChar8 *file = NULL;
  FcResult result;
  if (query) {
    p_FcConfigSubstitute(config, query, FcMatchPattern);
    p_FcDefaultSubstitute(query);
    match = p_FcFontMatch(config, query, &result);
  }
  if (!match ||
      p_FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch) {
    bench_skip("font_raster", "no_matching_font");
    goto close_patterns;
  }
  uint64_t match_time = bench_now() - start;
  bench_stats("font_match_sans", &match_time, 1);
  printf("FONT file=%s pixel_size=18 text=ASCII_32_to_126\n", file);
  FT_Library lib = NULL;
  FT_Face face = NULL;
  start = bench_now();
  if (p_FT_Init_FreeType(&lib) || p_FT_New_Face(lib, (char *)file, 0, &face) ||
      p_FT_Set_Pixel_Sizes(face, 0, 18)) {
    errno = EIO;
    bench_fail("freetype_face");
    goto close_face;
  }
  uint64_t loaded = bench_now() - start;
  bench_stats("font_face_load", &loaded, 1);
  uint64_t *times = calloc(samples, sizeof(*times));
  uint64_t checksum = 0;
  unsigned done = 0;
  if (!times) {
    bench_fail("font_samples");
    goto close_face;
  }
  for (; done < samples; done++) {
    start = bench_now();
    int bad = 0;
    for (unsigned c = 32; c < 127; c++) {
      if (p_FT_Load_Char(face, c, FT_LOAD_RENDER)) {
        bad = 1;
        break;
      }
      FT_Bitmap *b = &face->glyph->bitmap;
      for (unsigned y = 0; y < b->rows; y++)
        for (unsigned x = 0; x < b->width; x++)
          checksum += b->buffer[(ptrdiff_t)y * b->pitch + x];
    }
    if (bad) {
      errno = EIO;
      bench_fail("font_render");
      break;
    }
    times[done] = bench_now() - start;
  }
  if (done == samples) {
    bench_stats("font_raster_95glyphs", times, samples);
    printf("FONT checksum=%" PRIu64 "\n", checksum);
  }
  free(times);
close_face:
  if (face)
    p_FT_Done_Face(face);
  if (lib)
    p_FT_Done_FreeType(lib);
close_patterns:
  if (match)
    p_FcPatternDestroy(match);
  if (query)
    p_FcPatternDestroy(query);
  p_FcConfigDestroy(config);
  goto close_libs;
missing:
  bench_skip("fonts", "required_library_symbol_unavailable");
close_libs:
  if (ft)
    dlclose(ft);
  if (fc)
    dlclose(fc);
}
#undef LOAD
/* Run argv directly, never through a shell. Output is bounded on disk by the
 * child-side RLIMIT_FSIZE; only private scratch files/profiles are used. */
#include <sys/resource.h>
static int run_one(char **args, int output, uint64_t *elapsed) {
  fflush(NULL);
  uint64_t start = bench_now();
  pid_t p = fork();
  if (p < 0)
    return -1;
  if (!p) {
    if (setpgid(0, 0))
      _exit(126);
    struct rlimit limit = {16 * 1024 * 1024, 16 * 1024 * 1024};
    setrlimit(RLIMIT_FSIZE, &limit);
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
      dup2(null, 0);
      close(null);
    }
    if (dup2(output, 1) < 0 || dup2(output, 2) < 0)
      _exit(126);
    if (output > 2)
      close(output);
    execvp(args[0], args);
    _exit(127);
  }
  setpgid(p, p);
  uint64_t timeout = 30000000000ULL;
  const char *setting = getenv("AVORY_BENCH_TIMEOUT_MS");
  if (setting) {
    char *end;
    unsigned long ms = strtoul(setting, &end, 10);
    if (*setting && !*end && ms >= 10 && ms <= 120000)
      timeout = (uint64_t)ms * 1000000;
  }
  int status = 0;
  for (;;) {
    pid_t r = waitpid(p, &status, WNOHANG);
    if (r == p)
      break;
    if (r < 0 && errno != EINTR) {
      kill(-p, SIGKILL);
      kill(p, SIGKILL);
      return -1;
    }
    if (bench_now() - start >= timeout) {
      kill(-p, SIGKILL);
      kill(p, SIGKILL);
      while (waitpid(p, &status, 0) < 0 && errno == EINTR) {
      }
      errno = ETIMEDOUT;
      return -1;
    }
    struct timespec nap = {0, 1000000};
    nanosleep(&nap, NULL);
  }
  *elapsed = bench_now() - start;
  /* A workload must exit only when complete. Tear down any remaining group
   * members before deleting its private profile or starting another run. */
  kill(-p, SIGKILL);
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    printf("COMMAND exit_status=0x%x program=%s\n", status, args[0]);
    errno = ECHILD;
    return -1;
  }
  return 0;
}
int bench_command(unsigned runs, char **args) {
  FILE *out = tmpfile();
  uint64_t *times = calloc(runs, sizeof(*times));
  if (!out || !times) {
    if (out)
      fclose(out);
    free(times);
    bench_fail("command_alloc");
    return -1;
  }
  printf("COMMAND program=%s completion=process_exit poll_resolution_us=1000 "
         "timeout_default_s=30\n",
         args[0]);
  unsigned done = 0;
  for (; done < runs; done++) {
    if (ftruncate(fileno(out), 0) || fseek(out, 0, SEEK_SET) ||
        run_one(args, fileno(out), &times[done])) {
      bench_fail("command");
      break;
    }
    printf("COMMAND_RUN index=%u elapsed_us=%.3f\n", done, times[done] / 1e3);
  }
  if (done == runs)
    bench_stats("command_exit_completion", times, runs);
  else {
    rewind(out);
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, out);
    buf[n] = 0;
    printf("COMMAND diagnostic=%s\n", buf);
  }
  free(times);
  fclose(out);
  return done == runs ? 0 : -1;
}
static void remove_private(const char *path);
int bench_apps(unsigned samples, const char *self) {
  uint64_t *times = calloc(samples, sizeof(*times));
  if (!times) {
    bench_fail("dlopen_alloc");
    return -1;
  }
  unsigned i = 0;
  for (; i < samples; i++) {
    uint64_t t = bench_now();
    if (load_math()) {
      bench_skip("dlopen", "libm_unavailable");
      break;
    }
    times[i] = bench_now() - t;
  }
  if (i == samples)
    bench_stats("dlopen_resolve_call_close_libm_warm", times, samples);
  free(times);
  process_tree(samples, self);
  fonts(samples);
  const char *runner = getenv("AVORY_BENCH_COMPAT_RUNNER"),
             *program = getenv("AVORY_BENCH_COMPAT_PROGRAM");
  if (runner && !strcmp(runner, "none")) {
    bench_skip("compatibility", "disabled_by_configuration");
    return 0;
  }
  char companion[PATH_MAX];
  snprintf(companion, sizeof(companion), "%s", self);
  char *slash = strrchr(companion, '/');
  if (slash && (size_t)(slash - companion) + sizeof("/bench_compat.exe") <=
                   sizeof(companion))
    strcpy(slash, "/bench_compat.exe");
  else
    companion[0] = 0;
  if (!runner && !access("/usr/bin/wine", X_OK))
    runner = "/usr/bin/wine";
  if (!program && companion[0] && !access(companion, R_OK))
    program = companion;
  if (runner && program) {
    char prefix[] = "/tmp/avory-compat-XXXXXX";
    char *old = getenv("WINEPREFIX") ? strdup(getenv("WINEPREFIX")) : NULL;
    if (!mkdtemp(prefix) || setenv("WINEPREFIX", prefix, 1)) {
      free(old);
      bench_fail("compat_prefix");
      return -1;
    }
    printf("APP phase=compatibility_command runner=%s program=%s "
           "prefix=fresh_private first_run_includes_prefix_initialization\n",
           runner, program);
    char *args[] = {(char *)runner, (char *)program, NULL};
    bench_command(3, args);
    if (strstr(runner, "wine")) {
      FILE *out = tmpfile();
      uint64_t ignored;
      char *stop[] = {"wineserver", "-k", NULL},
           *wait[] = {"wineserver", "-w", NULL};
      if (!out || run_one(stop, out ? fileno(out) : -1, &ignored) ||
          run_one(wait, out ? fileno(out) : -1, &ignored))
        bench_fail("compat_server_cleanup");
      if (out)
        fclose(out);
    }
    if (old) {
      setenv("WINEPREFIX", old, 1);
      free(old);
    } else
      unsetenv("WINEPREFIX");
    remove_private(prefix);
  } else
    bench_skip("compatibility", "Wine_or_bench_compat.exe_unavailable_set_"
                                "AVORY_BENCH_COMPAT_RUNNER_and_PROGRAM");
  return 0;
}
static void remove_private(const char *path) {
  /* Chromium creates nested private profile contents. Use the existing
   * rm executable with an argv vector, never shell interpolation. */
  fflush(NULL);
  pid_t p = fork();
  if (!p) {
    execl("/bin/rm", "rm", "-rf", "--", path, (char *)NULL);
    _exit(127);
  }
  if (p < 0 || bench_wait(p))
    bench_fail("browser_profile_cleanup");
}
int bench_browser(unsigned runs, const char *base) {
  const char *browser = getenv("AVORY_BENCH_BROWSER");
  if (browser && !strcmp(browser, "none")) {
    bench_skip("browser", "disabled_by_configuration");
    return 0;
  }
  if (!browser) {
    const char *candidates[] = {"/usr/bin/chromium",
                                "/usr/bin/chromium-browser",
                                "/usr/bin/google-chrome", NULL};
    for (unsigned i = 0; candidates[i]; i++)
      if (!access(candidates[i], X_OK)) {
        browser = candidates[i];
        break;
      }
  }
  if (!browser) {
    bench_skip("browser", "Chromium_unavailable_set_AVORY_BENCH_BROWSER");
    return 0;
  }
  printf("BROWSER executable=%s page=local_DOM_canvas profile=fresh_private\n",
         browser);
  if (geteuid() == 0 &&
      (!getenv("AVORY_BENCH_BROWSER_NO_SANDBOX") ||
       strcmp(getenv("AVORY_BENCH_BROWSER_NO_SANDBOX"), "1"))) {
    bench_skip("browser",
               "root_requires_explicit_AVORY_BENCH_BROWSER_NO_SANDBOX_1");
    return 0;
  }
  char dir[PATH_MAX], html[PATH_MAX], url[3 * PATH_MAX + 8],
      profile[PATH_MAX + 64];
  if (snprintf(dir, sizeof(dir), "%s/avory-browser-XXXXXX", base) >=
      (int)sizeof(dir)) {
    errno = ENAMETOOLONG;
    bench_fail("browser_path");
    return -1;
  }
  if (!mkdtemp(dir)) {
    bench_fail("browser_mkdtemp");
    return -1;
  }
  if (snprintf(html, sizeof(html), "%s/workload.html", dir) >=
      (int)sizeof(html)) {
    bench_fail("browser_path");
    rmdir(dir);
    return -1;
  }
  FILE *page = fopen(html, "wx");
  if (!page) {
    bench_fail("browser_html");
    rmdir(dir);
    return -1;
  }
  fputs(
      "<!doctype html><meta charset=utf-8><title>Avory desktop "
      "benchmark</"
      "title><style>div{display:inline-block;width:64px;height:24px;font:16px "
      "sans-serif;transform:translateZ(0)}</style><output "
      "id=bench-result></output><canvas width=800 height=600></canvas><script>"
      "let sum=0;for(let i=0;i<1000000;i++)sum+=i;"
      "for(let i=0;i<1000;i++){let "
      "d=document.createElement('div');d.textContent='Row "
      "'+i;document.body.appendChild(d);}"
      "let c=document.querySelector('canvas').getContext('2d');for(let "
      "i=0;i<2000;i++){c.fillStyle='rgb('+(i%255)+',80,120)';c.fillRect(i%800,"
      "i%600,40,30);}"
      "document.body.offsetHeight;let "
      "o=document.getElementById('bench-result');o.textContent=String(sum);o."
      "setAttribute('data-complete','yes');</script>",
      page);
  if (fclose(page)) {
    bench_fail("browser_html_write");
    remove_private(dir);
    return -1;
  }
  char absolute[PATH_MAX];
  if (!realpath(html, absolute)) {
    bench_fail("browser_realpath");
    remove_private(dir);
    return -1;
  }
  strcpy(url, "file://");
  size_t used = 7;
  for (const unsigned char *s = (const unsigned char *)absolute; *s; s++) {
    if (isalnum(*s) || strchr("/-_.~", *s))
      url[used++] = *s;
    else {
      static const char hex[] = "0123456789ABCDEF";
      url[used++] = '%';
      url[used++] = hex[*s >> 4];
      url[used++] = hex[*s & 15];
    }
  }
  url[used] = 0;
  uint64_t *times = calloc(runs, sizeof(*times));
  if (!times) {
    bench_fail("browser_alloc");
    remove_private(dir);
    return -1;
  }
  unsigned done = 0;
  for (; done < runs; done++) {
    snprintf(profile, sizeof(profile), "--user-data-dir=%s/profile-%u", dir,
             done);
    char *args[16];
    unsigned a = 0;
    args[a++] = (char *)browser;
    args[a++] = "--headless";
    args[a++] = "--dump-dom";
    args[a++] = "--no-first-run";
    args[a++] = "--no-default-browser-check";
    args[a++] = "--disable-background-networking";
    args[a++] = "--disable-component-update";
    args[a++] = "--disable-sync";
    args[a++] = "--disable-extensions";
    args[a++] = profile;
    if (geteuid() == 0)
      args[a++] = "--no-sandbox";
    args[a++] = url;
    args[a] = NULL;
    FILE *out = tmpfile();
    if (!out) {
      bench_fail("browser_output");
      break;
    }
    int rc = run_one(args, fileno(out), &times[done]);
    rewind(out);
    char *data = calloc(1, 1024 * 1024 + 1);
    size_t len = data ? fread(data, 1, 1024 * 1024, out) : 0;
    fclose(out);
    int valid =
        data &&
        strstr(
            data,
            "id=\"bench-result\" data-complete=\"yes\">499999500000</output>");
    if (rc || !valid || len == 1024 * 1024) {
      if (data)
        printf("BROWSER diagnostic=%.512s\n", data);
      free(data);
      if (!rc)
        errno = EIO;
      bench_fail("browser_dom_completion");
      break;
    }
    free(data);
  }
  if (done == runs)
    bench_stats("browser_fresh_profile_local_DOM_canvas_exit", times, runs);
  free(times);
  remove_private(dir);
  return done == runs ? 0 : -1;
}
