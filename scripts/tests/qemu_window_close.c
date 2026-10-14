/* Send the window manager's close request to a named QEMU test window.
 * Run only on the isolated Xvfb display created by the diagnostic controller.
 * This does not destroy the window, signal QEMU, or access a physical device.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>

static Window find(Display *d, Window w, const char *name) {
  char *title = NULL;
  if (XFetchName(d, w, &title) && title) {
    int matches = strstr(title, name) != NULL;
    XFree(title);
    if (matches) return w;
  }
  Window root, parent, *children = NULL, result = None;
  unsigned count = 0;
  if (XQueryTree(d, w, &root, &parent, &children, &count)) {
    for (unsigned i = 0; i < count && !result; ++i)
      result = find(d, children[i], name);
    if (children) XFree(children);
  }
  return result;
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  Display *d = XOpenDisplay(NULL);
  if (!d) return 3;
  Window w = find(d, DefaultRootWindow(d), argv[1]);
  if (!w) { XCloseDisplay(d); return 4; }
  XEvent event = {0};
  event.xclient.type = ClientMessage;
  event.xclient.window = w;
  event.xclient.message_type = XInternAtom(d, "WM_PROTOCOLS", False);
  event.xclient.format = 32;
  event.xclient.data.l[0] = XInternAtom(d, "WM_DELETE_WINDOW", False);
  event.xclient.data.l[1] = CurrentTime;
  int sent = XSendEvent(d, w, False, NoEventMask, &event);
  XSync(d, False);
  printf("WM_DELETE_WINDOW sent=%d window=0x%lx title-match=%s\n", sent, w, argv[1]);
  XCloseDisplay(d);
  return sent ? 0 : 5;
}
