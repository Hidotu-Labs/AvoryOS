/* Headless desktop GL depth/stencil/MSAA oracle. Diagnostic, not a fix. */
#define _GNU_SOURCE
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
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <gbm.h>
#define SIDE 256
#define TILE 32
#define GRID (SIDE / TILE)
static int software, failures, incomplete;
static void fatal(const char *what) {
    fprintf(stderr,"ERROR %s GL=0x%x EGL=0x%x\n",what,glGetError(),eglGetError());
    exit(2);
}
static void check(const char *what) {
    GLenum error=glGetError();
    if(error) {fprintf(stderr,"ERROR %s GL=0x%x\n",what,error);exit(2);}
}
static int amd_node(void) {
    for(int n=128;n<192;n++) {
        char path[64],name[32]={0}; snprintf(path,sizeof path,"/dev/dri/renderD%d",n);
        int fd=open(path,O_RDWR|O_CLOEXEC); if(fd<0) continue;
        struct drm_version v={.name=name,.name_len=sizeof name-1};
        if(!drmIoctl(fd,DRM_IOCTL_VERSION,&v) && !strcmp(name,"amdgpu")) return fd;
        close(fd);
    }
    return -1;
}
static GLuint shader(GLenum kind,const char *text) {
    GLuint s=glCreateShader(kind); glShaderSource(s,1,&text,NULL); glCompileShader(s);
    GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok) {char log[2048];glGetShaderInfoLog(s,sizeof log,NULL,log);fprintf(stderr,"SHADER %s\n",log);exit(2);}
    return s;
}
static void color(unsigned frame,unsigned tile,unsigned pass,unsigned char c[4]) {
    c[0]=(frame*31+tile*13+pass*47)&255;
    c[1]=(frame*17+tile*23+pass*61)&255;
    c[2]=(frame*43+tile*7+pass*29)&255;c[3]=255;
}
static void expected_tile(unsigned char *p,unsigned tile,const unsigned char c[4]) {
    unsigned x=(tile%GRID)*TILE,y=(tile/GRID)*TILE;
    for(unsigned j=y;j<y+TILE;j++) for(unsigned i=x;i<x+TILE;i++)
        memcpy(p+((size_t)j*SIDE+i)*4,c,4);
}
static GLint zloc,cloc;
static void draw(float depth,const unsigned char c[4]) {
    glUniform1f(zloc,depth);
    glUniform4f(cloc,c[0]/255.0f,c[1]/255.0f,c[2]/255.0f,c[3]/255.0f);
    glDrawArrays(GL_TRIANGLES,0,3);
}
static void storage(GLuint rb,GLenum format,unsigned samples) {
    glBindRenderbuffer(GL_RENDERBUFFER,rb);
    if(samples>1) glRenderbufferStorageMultisample(GL_RENDERBUFFER,samples,format,SIDE,SIDE);
    else glRenderbufferStorage(GL_RENDERBUFFER,format,SIDE,SIDE);
    check("renderbuffer storage");
}
static void depth_case(GLenum format,const char *name,unsigned samples,int stencil) {
    GLuint fbo,resolve,cb,db,read_rb;
    unsigned char *expected=malloc(SIDE*SIDE*4),*actual=malloc(SIDE*SIDE*4);
    if(!expected || !actual) fatal("CPU oracle allocation");
    glGenFramebuffers(1,&fbo);glGenFramebuffers(1,&resolve);
    glGenRenderbuffers(1,&cb);glGenRenderbuffers(1,&db);glGenRenderbuffers(1,&read_rb);
    storage(cb,GL_RGBA8,samples);storage(db,format,samples);storage(read_rb,GL_RGBA8,1);
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,cb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,stencil?GL_DEPTH_STENCIL_ATTACHMENT:GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,db);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) fatal("depth framebuffer");
    glBindFramebuffer(GL_FRAMEBUFFER,resolve);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,read_rb);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) fatal("resolve framebuffer");
    unsigned frames=software?18:128;
    unsigned long long bad=0,checked=0;
    printf("BEGIN %s samples=%u frames=%u: depth rejection, stencil masks, partial clears, storage reuse\n",name,samples,frames);
    for(unsigned f=0;f<frames;f++) {
        if(f && !(f%16)) {storage(db,format,samples);storage(cb,GL_RGBA8,samples);}
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);glViewport(0,0,SIDE,SIDE);
        glDisable(GL_SCISSOR_TEST);glDisable(GL_STENCIL_TEST);
        glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glStencilMask(255);
        int reverse=f&1;glDepthFunc(reverse?GL_GREATER:GL_LESS);
        glClearDepth(reverse?0.0:1.0);glClearStencil(0);glClearColor(0,0,0,1);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|(stencil?GL_STENCIL_BUFFER_BIT:0));
        unsigned char c[4];color(f,0,0,c);
        if(stencil) {glEnable(GL_STENCIL_TEST);glStencilFunc(GL_ALWAYS,0,255);glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);}
        draw(reverse?0.35f:0.65f,c);
        glEnable(GL_SCISSOR_TEST);
        for(unsigned t=0;t<GRID*GRID;t++) {
            glScissor((t%GRID)*TILE,(t/GRID)*TILE,TILE,TILE);
            if(stencil) {glStencilMask(255);glStencilFunc(GL_ALWAYS,t&3,255);glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);}
            color(f,t,1,c);draw(reverse?0.65f:0.35f,c);expected_tile(expected,t,c);
            if(stencil) {
                /* Half the front draws pass stencil, half must be rejected. */
                glStencilMask(0);glStencilFunc(GL_EQUAL,(t&3)+(t&1),255);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
                color(f,t,2,c);draw(reverse?0.85f:0.15f,c);
                if(!(t&1)) expected_tile(expected,t,c);
                glStencilFunc(GL_ALWAYS,0,255);
            }
            /* This bright-green draw must fail depth for every sample. */
            unsigned char forbidden[4]={66,255,17,255};
            draw(reverse?0.05f:0.95f,forbidden);
        }
        /* Partial clear must affect only the selected tile. */
        unsigned t=f%(GRID*GRID);
        glScissor((t%GRID)*TILE,(t/GRID)*TILE,TILE,TILE);
        glStencilMask(255);glClear(GL_DEPTH_BUFFER_BIT|(stencil?GL_STENCIL_BUFFER_BIT:0));
        glDisable(GL_STENCIL_TEST);color(f,t,3,c);
        draw(reverse?0.15f:0.85f,c);expected_tile(expected,t,c);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,resolve);
        glBlitFramebuffer(0,0,SIDE,SIDE,0,0,SIDE,SIDE,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER,resolve);
        glReadPixels(0,0,SIDE,SIDE,GL_RGBA,GL_UNSIGNED_BYTE,actual);check("depth draw/resolve/readback");
        for(unsigned p=0;p<SIDE*SIDE;p++) {
            if(memcmp(actual+p*4,expected+p*4,4)) {
                if(bad<8) fprintf(stderr,"MISMATCH %s samples=%u frame=%u x=%u y=%u expected=%u,%u,%u,%u actual=%u,%u,%u,%u\n",
                    name,samples,f,p%SIDE,p/SIDE,expected[p*4],expected[p*4+1],expected[p*4+2],expected[p*4+3],actual[p*4],actual[p*4+1],actual[p*4+2],actual[p*4+3]);
                bad++;
            }
            checked++;
        }
        if(bad) break;
    }
    printf("%s %s samples=%u pixels=%llu mismatches=%llu\n",bad?"FAIL":"PASS",name,samples,checked,bad);
    failures+=!!bad;
    glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glBindFramebuffer(GL_FRAMEBUFFER,0);
    glDeleteFramebuffers(1,&fbo);glDeleteFramebuffers(1,&resolve);
    glDeleteRenderbuffers(1,&cb);glDeleteRenderbuffers(1,&db);glDeleteRenderbuffers(1,&read_rb);
    free(expected);free(actual);
}
int main(int argc,char **argv) {
    software=argc==2&&!strcmp(argv[1],"--software");
    if(argc!=1&&!software) {fprintf(stderr,"Usage: %s [--software]\n",argv[0]);return 2;}
    setvbuf(stdout,NULL,_IOLBF,0);
    if(software) {
        setenv("LIBGL_ALWAYS_SOFTWARE","1",1);
        if(!access("/usr/share/glvnd/egl_vendor.d/50_mesa.json",R_OK))
            setenv("__EGL_VENDOR_LIBRARY_FILENAMES","/usr/share/glvnd/egl_vendor.d/50_mesa.json",1);
    }
    int fd=software?-1:amd_node();
    if(!software&&fd<0) {fprintf(stderr,"No AMD render node\n");return 2;}
    struct gbm_device *gbm=software?NULL:gbm_create_device(fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if(!platform||(!software&&!gbm)) fatal("GBM/EGL setup");
    EGLDisplay dpy=platform(software?EGL_PLATFORM_SURFACELESS_MESA:EGL_PLATFORM_GBM_KHR,gbm,NULL);
    EGLint major,minor,n;EGLConfig cfg;
    if(!eglInitialize(dpy,&major,&minor)||!eglBindAPI(EGL_OPENGL_API)) fatal("EGL initialize");
    EGLint attrs[]={EGL_SURFACE_TYPE,software?EGL_PBUFFER_BIT:EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    if(!eglChooseConfig(dpy,attrs,&cfg,1,&n)||n!=1) fatal("EGL config");
    EGLint ca[]={EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,3,EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,EGL_NONE};
    EGLContext ctx=eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,ca);
    if(ctx==EGL_NO_CONTEXT||!eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx)) fatal("EGL context");
    const char *renderer=(const char*)glGetString(GL_RENDERER);
    printf("GPU depth oracle v1\nRenderer: %s\nGL: %s\n",renderer?renderer:"null",glGetString(GL_VERSION));
    if(!renderer||(software?(!strstr(renderer,"llvmpipe")&&!strstr(renderer,"softpipe")):!strstr(renderer,"radeonsi"))) fatal("unexpected renderer");
    GLuint vs=shader(GL_VERTEX_SHADER,"#version 330 core\nuniform float z;void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));gl_Position=vec4(p*2.0-1.0,z*2.0-1.0,1);}");
    GLuint fs=shader(GL_FRAGMENT_SHADER,"#version 330 core\nuniform vec4 paint;out vec4 color;void main(){color=paint;}");
    GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
    GLint linked;glGetProgramiv(program,GL_LINK_STATUS,&linked);if(!linked) fatal("shader program");
    glUseProgram(program);zloc=glGetUniformLocation(program,"z");cloc=glGetUniformLocation(program,"paint");
    GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glDisable(GL_DITHER);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);glPixelStorei(GL_PACK_ALIGNMENT,1);
    GLint max_samples;glGetIntegerv(GL_MAX_SAMPLES,&max_samples);
    for(unsigned samples=1;samples<=4;samples*=4) {
        if(samples>1&&max_samples<4) {puts("INCOMPLETE 4x MSAA unavailable");incomplete++;continue;}
        depth_case(GL_DEPTH_COMPONENT16,"depth16",samples,0);
        depth_case(GL_DEPTH_COMPONENT24,"depth24",samples,0);
        depth_case(GL_DEPTH_COMPONENT32F,"depth32f",samples,0);
        depth_case(GL_DEPTH24_STENCIL8,"depth24-stencil8",samples,1);
    }
    glDeleteVertexArrays(1,&vao);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);
    eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(dpy,ctx);eglTerminate(dpy);
    if(gbm) gbm_device_destroy(gbm);
    if(fd>=0) close(fd);
    printf("SUMMARY failures=%d incomplete=%d mode=%s\n",failures,incomplete,software?"software-control":"RadeonSI-depth-before-display");
    return failures?1:incomplete?2:0;
}
