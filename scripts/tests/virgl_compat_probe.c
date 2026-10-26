/* Legacy fixed-function rendering used by vanilla Minecraft 1.16.x. */
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(c) do {if(!(c)){printf("VIRGL_COMPAT: FAIL line=%d gl=%x egl=%x\n",__LINE__,glGetError(),eglGetError());return 1;}}while(0)
struct vertex {float x,y,z;unsigned char color[4];float u,v;short light[2];};
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);
 int fd=open("/dev/dri/card0",O_RDWR);CHECK(fd>=0);
 struct gbm_device *gbm=gbm_create_device(fd);CHECK(gbm);
 EGLDisplay d=eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,gbm,NULL);
 CHECK(eglInitialize(d,NULL,NULL)&&eglBindAPI(EGL_OPENGL_API));
 EGLint attributes[]={EGL_SURFACE_TYPE,EGL_DONT_CARE,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};
 EGLConfig config;EGLint n;CHECK(eglChooseConfig(d,attributes,&config,1,&n)&&n);
 EGLContext ctx=eglCreateContext(d,config,EGL_NO_CONTEXT,NULL);CHECK(ctx!=EGL_NO_CONTEXT);
 CHECK(eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx));
 const char *renderer=(const char*)glGetString(GL_RENDERER);
 printf("VIRGL_COMPAT: renderer=%s\n",renderer);CHECK(renderer&&strstr(renderer,"virgl"));
 GLuint fbo,color,depth;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
 glGenRenderbuffers(1,&color);glBindRenderbuffer(GL_RENDERBUFFER,color);glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,64,64);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,color);
 glGenRenderbuffers(1,&depth);glBindRenderbuffer(GL_RENDERBUFFER,depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,64,64);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
 CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
 GLuint textures[2];glGenTextures(2,textures);
 for(unsigned i=0;i<2;i++){
  glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,textures[i]);glEnable(GL_TEXTURE_2D);
  unsigned char white[8*8*4];memset(white,255,sizeof(white));
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
  glGenerateMipmap(GL_TEXTURE_2D);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
 }
 glViewport(0,0,64,64);glDisable(GL_DITHER);glEnable(GL_DEPTH_TEST);glEnable(GL_ALPHA_TEST);glAlphaFunc(GL_GREATER,.1f);
 glEnable(GL_FOG);GLfloat fog[]={0,0,1,1};glFogfv(GL_FOG_COLOR,fog);glFogi(GL_FOG_MODE,GL_LINEAR);glFogf(GL_FOG_START,0);glFogf(GL_FOG_END,1);
 GLuint vbo[64],ibo;glGenBuffers(64,vbo);glGenBuffers(1,&ibo);
 glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ibo);unsigned short indices[]={0,1,2,3};glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices,GL_STATIC_DRAW);
 glEnableClientState(GL_VERTEX_ARRAY);glEnableClientState(GL_COLOR_ARRAY);
 for(unsigned frame=0;frame<32;frame++){
  glClearColor(1,0,1,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  for(unsigned chunk=0;chunk<64;chunk++){
   float x=-1+(chunk%8)*.25f,y=-1+(chunk/8)*.25f;
   unsigned char r=frame%2?255:0,g=frame%2?0:255;
   struct vertex vertices[]={
    {x,y,-.5f,{r,g,0,255},0,0,{0,0}},
    {x+.25f,y,-.5f,{r,g,0,255},1,0,{1,0}},
    {x+.25f,y+.25f,-.5f,{r,g,0,255},1,1,{1,1}},
    {x,y+.25f,-.5f,{r,g,0,255},0,1,{0,1}}};
   glBindBuffer(GL_ARRAY_BUFFER,vbo[chunk]);
   /* Exercise both orphaning and writes to retained mapped chunk buffers. */
   glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),frame%2?NULL:vertices,GL_DYNAMIC_DRAW);
   if(frame%2){void *p=glMapBufferRange(GL_ARRAY_BUFFER,0,sizeof(vertices),GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT);CHECK(p);memcpy(p,vertices,sizeof(vertices));CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));}
   glVertexPointer(3,GL_FLOAT,sizeof(struct vertex),(void*)0);glColorPointer(4,GL_UNSIGNED_BYTE,sizeof(struct vertex),(void*)12);
   glClientActiveTexture(GL_TEXTURE0);glEnableClientState(GL_TEXTURE_COORD_ARRAY);glTexCoordPointer(2,GL_FLOAT,sizeof(struct vertex),(void*)16);
   glClientActiveTexture(GL_TEXTURE1);glEnableClientState(GL_TEXTURE_COORD_ARRAY);glTexCoordPointer(2,GL_SHORT,sizeof(struct vertex),(void*)24);
   GLsizei count=4;const void *offset=NULL;glMultiDrawElements(GL_QUADS,&count,GL_UNSIGNED_SHORT,&offset,1);
  }
  unsigned char pixels[64*64*4];glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);CHECK(glGetError()==GL_NO_ERROR);
  for(unsigned chunk=0;chunk<64;chunk++){
   unsigned char *p=pixels+4*((chunk/8*8+4)*64+chunk%8*8+4);
   unsigned expected_r=frame%2?128:0,expected_g=frame%2?0:128;
   if(p[0]+2<expected_r||p[0]>expected_r+2||p[1]+2<expected_g||p[1]>expected_g+2||p[2]<126||p[2]>130){
    printf("VIRGL_COMPAT: frame=%u chunk=%u pixel=%u,%u,%u\n",frame,chunk,p[0],p[1],p[2]);CHECK(0);
   }
  }
 }
 puts("VIRGL_COMPAT: 32 frames x 64 rebuilt chunks, indexed quads, packed colors, two textures, mipmaps, depth/alpha and fog pixels PASS");
 glDeleteBuffers(64,vbo);glDeleteBuffers(1,&ibo);glDeleteTextures(2,textures);glDeleteFramebuffers(1,&fbo);glDeleteRenderbuffers(1,&color);glDeleteRenderbuffers(1,&depth);
 eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(d,ctx);eglTerminate(d);gbm_device_destroy(gbm);close(fd);return 0;
}
