#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct input_event {struct timeval t;uint16_t type,code;int32_t value;};
#define CHECK(c) do { if(!(c)){printf("MOUSE: FAIL line=%d egl=%x\n",__LINE__,eglGetError());return 2;} } while(0)
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);
 Display *d=XOpenDisplay(NULL);CHECK(d);
 Window root=DefaultRootWindow(d),child,ret;int x,y,wx,wy,bx,by;unsigned mask;
 EGLDisplay ed=eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR,d,NULL);CHECK(ed!=EGL_NO_DISPLAY);
 CHECK(eglInitialize(ed,NULL,NULL));CHECK(eglBindAPI(EGL_OPENGL_ES_API));
 EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
 EGLConfig cfg;EGLint count,visual;CHECK(eglChooseConfig(ed,attrs,&cfg,1,&count)&&count);
 CHECK(eglGetConfigAttrib(ed,cfg,EGL_NATIVE_VISUAL_ID,&visual));
 XVisualInfo tmpl={.visualid=visual};XVisualInfo *vi=XGetVisualInfo(d,VisualIDMask,&tmpl,&count);CHECK(vi);
 XSetWindowAttributes wa={.colormap=XCreateColormap(d,root,vi->visual,AllocNone),.override_redirect=True};
 Window win=XCreateWindow(d,root,40,40,320,240,0,vi->depth,InputOutput,vi->visual,CWColormap|CWOverrideRedirect,&wa);
 XMapWindow(d,win);XSync(d,False);
 EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
 EGLContext ctx=eglCreateContext(ed,cfg,EGL_NO_CONTEXT,ca);CHECK(ctx!=EGL_NO_CONTEXT);
 EGLSurface surf=eglCreateWindowSurface(ed,cfg,win,NULL);CHECK(surf!=EGL_NO_SURFACE);
 CHECK(eglMakeCurrent(ed,surf,surf,ctx));eglSwapInterval(ed,0);
 const char *renderer=(const char*)glGetString(GL_RENDERER);printf("MOUSE: renderer=%s\n",renderer);CHECK(renderer&&strstr(renderer,"virgl"));
 XSelectInput(d,root,PointerMotionMask);XSync(d,False);
 int xi,event,error;CHECK(XQueryExtension(d,"XInputExtension",&xi,&event,&error));
 int xi_major=2,xi_minor=2;CHECK(XIQueryVersion(d,&xi_major,&xi_minor)==Success);
 unsigned char bits[XIMaskLen(XI_RawButtonRelease)]={0};XISetMask(bits,XI_RawButtonPress);XISetMask(bits,XI_RawButtonRelease);
 XIEventMask em={.deviceid=XIAllMasterDevices,.mask_len=sizeof(bits),.mask=bits};CHECK(XISelectEvents(d,root,&em,1)==Success);
 XQueryPointer(d,root,&ret,&child,&bx,&by,&wx,&wy,&mask);
 printf("MOUSE: START pos=%d,%d\n",bx,by);
 int ready=open("/dev/kmsg",O_WRONLY);CHECK(ready>=0);dprintf(ready,"MOUSE: INPUT READY\n");close(ready);
 int ev=open("/dev/input/event1",O_RDONLY|O_NONBLOCK),raw=0,motion=0,press=0,release=0,hover=0,frames=0,px=bx,py=by;
 CHECK(ev>=0);
 int stress=getenv("VIRGL_STRESS")!=NULL,iterations=stress?600:150;
 int width=320,height=240,checkpoint_motion=0;
 for(int n=0;n<iterations;n++){
  if(stress&&n%20==0){
   int mode=(n/20)%3;
   width=mode==0?320:mode==1?640:DisplayWidth(d,DefaultScreen(d));
   height=mode==0?240:mode==1?480:DisplayHeight(d,DefaultScreen(d));
   XMoveResizeWindow(d,win,mode==2?0:40,mode==2?0:40,width,height);XSync(d,False);
  }
  /* Distinct upper/lower colors catch texture-origin mistakes in scanout.
   * A moving stripe forces continuing glamor, DRI3 and display updates. */
  glDisable(GL_SCISSOR_TEST);glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);glScissor(0,height/2,width,height-height/2);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  glScissor(80+(n%100),0,16,height);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);
  CHECK(eglSwapBuffers(ed,surf));frames++;
  if(n%100==10){XSync(d,False);XImage *im=XGetImage(d,win,10,10,1,1,AllPlanes,ZPixmap);CHECK(im);unsigned long pixel=XGetPixel(im,0,0);printf("MOUSE: X11 window pixel=%lx\n",pixel);CHECK(pixel==0xff0000);XDestroyImage(im);}
  struct input_event events[64];int got=read(ev,events,sizeof(events));if(got>0)raw+=got/sizeof(*events);
  while(XPending(d)){XEvent event;XNextEvent(d,&event);if(event.type==MotionNotify)motion++;if(event.type==GenericEvent&&event.xcookie.extension==xi&&XGetEventData(d,&event.xcookie)){XIRawEvent *button=event.xcookie.data;if(button->detail==1){if(event.xcookie.evtype==XI_RawButtonPress)press++;if(event.xcookie.evtype==XI_RawButtonRelease)release++;}XFreeEventData(d,&event.xcookie);}}
  XQueryPointer(d,root,&ret,&child,&x,&y,&wx,&wy,&mask);
  if(x!=px||y!=py){motion++;if(!press)hover++;}px=x;py=y;
  if(n%25==0)printf("MOUSE: FRAME n=%d pos=%d,%d motion=%d press=%d release=%d\n",n,x,y,motion,press,release);
  if(n%50==49){int live=open("/dev/kmsg",O_WRONLY);if(live>=0){dprintf(live,"MOUSE: CHECKPOINT n=%d raw=%d motion=%d press=%d release=%d\n",n,raw,motion,press,release);close(live);}CHECK(motion-checkpoint_motion>=5);checkpoint_motion=motion;}
  if(n==125){int live=open("/dev/kmsg",O_WRONLY);CHECK(live>=0);dprintf(live,"MOUSE: CAPTURE LIVE\n");close(live);}
  usleep(100000);
 }
 printf("MOUSE: END pos=%d,%d raw=%d hover=%d motion=%d press=%d release=%d frames=%d\n",x,y,raw,hover,motion,press,release,frames);
 CHECK(raw&&hover>=10&&motion>=20&&press>=3&&release>=3&&frames==iterations);
 struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
 glDisable(GL_SCISSOR_TEST);
 for(int n=0;n<120;n++){glClearColor(n%2,0,1-(n%2),1);glClear(GL_COLOR_BUFFER_BIT);CHECK(eglSwapBuffers(ed,surf));}
 glFinish();clock_gettime(CLOCK_MONOTONIC,&end);
 long long ns=(end.tv_sec-start.tv_sec)*1000000000LL+end.tv_nsec-start.tv_nsec;
 printf("MOUSE: BENCH frames=120 elapsed_us=%lld\n",ns/1000);
 sleep(2); /* Host QMP captures the final displayed frame before teardown. */
 eglMakeCurrent(ed,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroySurface(ed,surf);eglDestroyContext(ed,ctx);eglTerminate(ed);
 XDestroyWindow(d,win);XFree(vi);close(ev);XCloseDisplay(d);return 0;
}
