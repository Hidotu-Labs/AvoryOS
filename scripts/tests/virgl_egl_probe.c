#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <X11/Xlib.h>
#include <wayland-client.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(c) do {if(!(c)){printf("VIRGL_MESA: FAIL line=%d egl=%x gl=%x\n",__LINE__,eglGetError(),glGetError());return 1;}}while(0)
int main(int argc, char **argv) {
 setvbuf(stdout,NULL,_IOLBF,0);
 int fd=-1;struct gbm_device *gbm=NULL;Display *x11=NULL;struct wl_display *wayland=NULL;
 EGLDisplay d=EGL_NO_DISPLAY;
 if(argc>1 && !strcmp(argv[1], "x11")) {
  x11=XOpenDisplay(NULL);CHECK(x11);d=eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR,x11,NULL);
 } else if(argc>1 && !strcmp(argv[1], "wayland")) {
  wayland=wl_display_connect(NULL);CHECK(wayland);d=eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,wayland,NULL);
 } else {
  fd=open("/dev/dri/card0",O_RDWR);CHECK(fd>=0);gbm=gbm_create_device(fd);CHECK(gbm);
  d=eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,gbm,NULL);
 }
 CHECK(d!=EGL_NO_DISPLAY);
 EGLint major,minor;CHECK(eglInitialize(d,&major,&minor));
 CHECK(eglBindAPI(EGL_OPENGL_ES_API));
 EGLint attrs[]={EGL_SURFACE_TYPE,EGL_DONT_CARE,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
 EGLConfig config;EGLint n;CHECK(eglChooseConfig(d,attrs,&config,1,&n)&&n);
 EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
 EGLContext ctx=eglCreateContext(d,config,EGL_NO_CONTEXT,ca);CHECK(ctx!=EGL_NO_CONTEXT);
 CHECK(eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx));
 const char *renderer=(const char*)glGetString(GL_RENDERER);printf("VIRGL_MESA: renderer=%s\n",renderer);CHECK(renderer&&strstr(renderer,"virgl"));
 GLuint tex,fbo;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
 glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
 CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
 glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
 unsigned char pixel[4]={0};glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
 printf("VIRGL_MESA: pixel=%u,%u,%u,%u error=%x\n",pixel[0],pixel[1],pixel[2],pixel[3],glGetError());
 CHECK(pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==255);
 /* Desktop compositors can retain hundreds of textures and buffer objects.
  * Keep them live together so a small per-file GEM table cannot hide behind
  * handle reuse in short create/destroy tests. */
 GLuint retained[512];glGenTextures(512,retained);
 for(unsigned i=0;i<512;i++){
  glBindTexture(GL_TEXTURE_2D,retained[i]);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,32,32,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
  GLenum allocation_error=glGetError();
  if(allocation_error)printf("VIRGL_MESA: retained texture=%u allocation_error=%x\n",i,allocation_error);
  CHECK(allocation_error==GL_NO_ERROR);
  if(i%32==31)glFinish();
 }
 glFinish();CHECK(glGetError()==GL_NO_ERROR);
 puts("VIRGL_MESA: 512 retained textures PASS");
 glDeleteTextures(512,retained);
 eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(d,ctx);eglTerminate(d);if(gbm)gbm_device_destroy(gbm);if(fd>=0)close(fd);if(x11)XCloseDisplay(x11);if(wayland)wl_display_disconnect(wayland);
 puts("VIRGL_MESA: PASS");return 0;
}
