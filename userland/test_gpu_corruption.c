/* Fog-independent, offscreen RadeonSI resource upload/readback oracle.
 * glReadPixels is the required API synchronization, not a driver workaround.
 * No scanout/PRIME buffer is involved. --software validates the CPU oracle;
 * only a real RadeonSI run can reproduce or clear an AvoryOS GPU defect. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <gbm.h>
#include <zlib.h>

enum { SIDE=256, TILE=64, GRID=4, SOURCE=256, SOURCES=4, SAVED_LIMIT=8 };
static unsigned frames=300, saved, failing_frames;
static int software, serialize, integer_source, inject=-1;
static const char *output="gpu-corruption", *workload="mixed";
static void die(const char *s) { fprintf(stderr,"ERROR: %s: errno=%d GL=0x%x EGL=0x%x\n",s,errno,glGetError(),eglGetError());exit(2); }
static void check(const char *s) { GLenum error=glGetError();if(error!=GL_NO_ERROR){fprintf(stderr,"ERROR: %s GL=0x%x\n",s,error);exit(2);} }
static void write_all(FILE *f,const void *p,size_t n) { if(fwrite(p,1,n,f)!=n)die("write capture"); }
static void be32(unsigned char *p,uint32_t n) { p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n; }
static void chunk(FILE *f,const char type[4],const void *p,unsigned n) {
    unsigned char word[4];be32(word,n);write_all(f,word,4);write_all(f,type,4);write_all(f,p,n);
    uLong crc=crc32(0,(const Bytef*)type,4);if(n)crc=crc32(crc,p,n);be32(word,crc);write_all(f,word,4);
}
/* PNG rows are top-to-bottom; OpenGL framebuffer rows are bottom-to-top. */
static void png(const char *path,const unsigned char *rgba) {
    size_t row=SIDE*4+1, bytes=row*SIDE;
    unsigned char *raw=malloc(bytes);uLongf size=compressBound(bytes);unsigned char *z=malloc(size);
    if(!raw||!z)die("PNG allocation");
    for(unsigned y=0;y<SIDE;y++){raw[y*row]=0;memcpy(raw+y*row+1,rgba+(SIDE-1-y)*SIDE*4,SIDE*4);}
    if(compress2(z,&size,raw,bytes,1)!=Z_OK)die("PNG compression");
    FILE *f=fopen(path,"wb");if(!f)die("open PNG");
    static const unsigned char magic[]={137,80,78,71,13,10,26,10};write_all(f,magic,8);
    unsigned char hdr[13]={0};be32(hdr,SIDE);be32(hdr+4,SIDE);hdr[8]=8;hdr[9]=6;
    chunk(f,"IHDR",hdr,13);chunk(f,"IDAT",z,size);chunk(f,"IEND",NULL,0);
    if(fclose(f))die("close PNG");
    free(raw);free(z);
}
static int amd_node(void) {
    for(int n=128;n<160;n++) {
        char path[64],name[32]={0};snprintf(path,sizeof(path),"/dev/dri/renderD%d",n);
        int fd=open(path,O_RDWR|O_CLOEXEC);if(fd<0)continue;
        struct drm_version v={.name=name,.name_len=sizeof(name)-1};
        if(!drmIoctl(fd,DRM_IOCTL_VERSION,&v)&&!strcmp(name,"amdgpu"))return fd;
        close(fd);
    }
    return -1;
}
static GLuint shader(GLenum type,const char *source) {
    GLuint s=glCreateShader(type);glShaderSource(s,1,&source,NULL);glCompileShader(s);
    GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof(log),NULL,log);fprintf(stderr,"%s\n",log);die("shader compile");}return s;
}
static uint32_t seed_for(unsigned frame,unsigned draw) { return frame*17u+draw*97u+13u; }
static void texel(unsigned x,unsigned y,uint32_t seed,unsigned char *p) {
    p[0]=(x*13u+y*7u+seed*31u)&255;p[1]=(x*3u+y*11u+seed*47u)&255;
    p[2]=(x*19u+y*5u+seed*61u)&255;p[3]=255;
}
static void mask_for(unsigned frame,unsigned draw,GLuint p[4]) {
    p[0]=(frame*3u+draw*11u)&255;p[1]=(frame*7u+draw*17u)&255;p[2]=(frame*13u+draw*23u)&255;p[3]=0;
}
static unsigned number(const char *s) { char *end;errno=0;unsigned long n=strtoul(s,&end,10);if(errno||!*s||*end||n>100000)die("numeric option");return n; }
static uint64_t ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int main(int argc,char **argv) {
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--software"))software=1;
        else if(!strcmp(argv[i],"--integer-source"))integer_source=1;
        else if(!strcmp(argv[i],"--serialize"))serialize=1;
        else if(!strcmp(argv[i],"--frames")&&i+1<argc)frames=number(argv[++i]);
        else if(!strcmp(argv[i],"--output")&&i+1<argc)output=argv[++i];
        else if(!strcmp(argv[i],"--case")&&i+1<argc)workload=argv[++i];
        else if(!strcmp(argv[i],"--inject-readback-error")&&i+1<argc)inject=number(argv[++i]);
        else {fprintf(stderr,"Usage: %s [--software] [--frames 300] [--case mixed|texture|stream] [--output DIR] [--integer-source] [--serialize] [--inject-readback-error FRAME]\n",argv[0]);return 2;}
    }
    if(!frames|| (strcmp(workload,"mixed")&&strcmp(workload,"texture")&&strcmp(workload,"stream")))die("frames/case");
    if(mkdir(output,0755)&&errno!=EEXIST)die("capture directory");
    char path[1024];if(snprintf(path,sizeof(path),"%s/results.csv",output)>=(int)sizeof(path))die("output path too long");
    FILE *csv=fopen(path,"w");if(!csv)die("results.csv");
    int fd=software?-1:amd_node();if(!software&&fd<0){fputs("ERROR: no AMDGPU render node; RadeonSI test did not run\n",stderr);fclose(csv);return 2;}
    if(software) {
        setenv("LIBGL_ALWAYS_SOFTWARE","1",1);
        setenv("GALLIUM_DRIVER","llvmpipe",1);
        if(!getenv("__EGL_VENDOR_LIBRARY_FILENAMES") &&
           !access("/usr/share/glvnd/egl_vendor.d/50_mesa.json",R_OK))
            setenv("__EGL_VENDOR_LIBRARY_FILENAMES","/usr/share/glvnd/egl_vendor.d/50_mesa.json",1);
    }
    struct gbm_device *gbm=software?NULL:gbm_create_device(fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if(!platform||(!software&&!gbm))die("EGL/GBM platform");
    EGLDisplay dpy=platform(software?EGL_PLATFORM_SURFACELESS_MESA:EGL_PLATFORM_GBM_KHR,gbm,NULL);
    EGLint major,minor,n;
    if(!eglInitialize(dpy,&major,&minor)||!eglBindAPI(EGL_OPENGL_API))die("EGL initialize");
    EGLint cfg[]={EGL_SURFACE_TYPE,software?EGL_PBUFFER_BIT:EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};EGLConfig config;
    if(!eglChooseConfig(dpy,cfg,&config,1,&n)||n!=1)die("EGL config");
    EGLint attr[]={EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,3,EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,EGL_NONE};
    EGLContext ctx=eglCreateContext(dpy,config,EGL_NO_CONTEXT,attr);
    if(ctx==EGL_NO_CONTEXT||!eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx))die("surfaceless GL context");
    const char *renderer=(const char*)glGetString(GL_RENDERER);
    printf("GPU corruption oracle v1\nRenderer: %s\nVendor: %s\nGL: %s\nEGL: %s\ncase=%s frames=%u mode=%s serialize=%d injected_frame=%d\n",renderer,(const char*)glGetString(GL_VENDOR),(const char*)glGetString(GL_VERSION),eglQueryString(dpy,EGL_VERSION),workload,frames,software?"software-oracle-control":"RadeonSI-before-display",serialize,inject);
    printf("texture_source=%s\n",integer_source?"RGBA8UI":"RGBA8-normalized");
    if(!renderer||(!software&&!strstr(renderer,"radeonsi"))||(software&&!strstr(renderer,"llvmpipe")))die("unexpected renderer");
    if(inject>=0)fputs("TEST-ONLY: CPU readback error injection is enabled; not evidence of GPU corruption\n",stderr);
    GLuint vs=shader(GL_VERTEX_SHADER,"#version 330 core\nlayout(location=0) in vec2 p;void main(){gl_Position=vec4(p,0,1);}");
    GLuint fs=shader(GL_FRAGMENT_SHADER,integer_source?
        "#version 330 core\nuniform usampler2D source;uniform ivec2 origin;uniform uvec4 mask;out vec4 color;void main(){ivec2 p=ivec2(gl_FragCoord.xy)-origin;uvec4 b=texelFetch(source,p,0)^mask;color=vec4(b)/255.0;}":
        "#version 330 core\nuniform sampler2D source;uniform ivec2 origin;uniform uvec4 mask;out vec4 color;void main(){ivec2 p=ivec2(gl_FragCoord.xy)-origin;uvec4 b=uvec4(round(texelFetch(source,p,0)*255.0))^mask;color=vec4(b)/255.0;}");
    GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)die("program link");glUseProgram(program);
    GLint origin=glGetUniformLocation(program,"origin"),mask=glGetUniformLocation(program,"mask");glUniform1i(glGetUniformLocation(program,"source"),0);
    GLuint vao,vbo,fbo,target,textures[SOURCES];glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);GLfloat quad[]={-1,-1,1,-1,-1,1,1,1};
    glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STREAM_DRAW);glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,NULL);
    glGenTextures(1,&target);glBindTexture(GL_TEXTURE_2D,target);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,SIDE,SIDE,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);glDrawBuffer(GL_COLOR_ATTACHMENT0);glReadBuffer(GL_COLOR_ATTACHMENT0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)die("FBO completeness");
    glDisable(GL_BLEND);glDisable(GL_DITHER);glDisable(GL_DEPTH_TEST);glDisable(GL_SCISSOR_TEST);glDisable(GL_CULL_FACE);glDisable(GL_FRAMEBUFFER_SRGB);glDisable(GL_MULTISAMPLE);
    glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    unsigned char *upload=malloc(SOURCE*SOURCE*4),*actual=malloc(SIDE*SIDE*4),*expected=malloc(SIDE*SIDE*4);if(!upload||!actual||!expected)die("CPU buffers");
    for(unsigned y=0;y<SOURCE;y++)for(unsigned x=0;x<SOURCE;x++)texel(x,y,0,upload+(y*SOURCE+x)*4);
    glActiveTexture(GL_TEXTURE0);glGenTextures(SOURCES,textures);
    for(unsigned t=0;t<SOURCES;t++){glBindTexture(GL_TEXTURE_2D,textures[t]);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexImage2D(GL_TEXTURE_2D,0,integer_source?GL_RGBA8UI:GL_RGBA8,SOURCE,SOURCE,0,integer_source?GL_RGBA_INTEGER:GL_RGBA,GL_UNSIGNED_BYTE,upload);}
    check("setup");uint64_t started=ms(),bad_total=0;
    fprintf(csv,"frame,bad_pixels,actual_crc32,expected_crc32,elapsed_ms\n");
    for(unsigned frame=0;frame<frames;frame++) {
        for(unsigned draw=0;draw<GRID*GRID;draw++) {
            uint32_t seed=!strcmp(workload,"stream")?0:seed_for(frame,draw);
            GLuint m[4];mask_for(frame,draw,m);
            glBindTexture(GL_TEXTURE_2D,textures[draw%SOURCES]);
            if(strcmp(workload,"stream")){
                for(unsigned y=0;y<SOURCE;y++)for(unsigned x=0;x<SOURCE;x++)texel(x,y,seed,upload+(y*SOURCE+x)*4);
                glTexSubImage2D(GL_TEXTURE_2D,0,0,0,SOURCE,SOURCE,integer_source?GL_RGBA_INTEGER:GL_RGBA,GL_UNSIGNED_BYTE,upload);
            }
            if(strcmp(workload,"texture")){
                glBufferData(GL_ARRAY_BUFFER,sizeof(quad),NULL,GL_STREAM_DRAW);glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(quad),quad);
            }
            int ox=(draw%GRID)*TILE,oy=(draw/GRID)*TILE;glViewport(ox,oy,TILE,TILE);glUniform2i(origin,ox,oy);glUniform4uiv(mask,1,m);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            for(unsigned y=0;y<TILE;y++)for(unsigned x=0;x<TILE;x++){
                unsigned char *p=expected+((oy+y)*SIDE+ox+x)*4;texel(x,y,seed,p);for(unsigned k=0;k<4;k++)p[k]^=m[k];
            }
            glFlush();if(serialize)glFinish(); /* optional experiment only */
        }
        glReadPixels(0,0,SIDE,SIDE,GL_RGBA,GL_UNSIGNED_BYTE,actual);check("draw/readback");
        if((int)frame==inject)actual[4*(17*SIDE+23)]^=255;
        unsigned bad=0,first=0;for(unsigned p=0;p<SIDE*SIDE;p++)if(memcmp(actual+p*4,expected+p*4,4)){if(!bad)first=p;bad++;}
        unsigned ac=crc32(0,actual,SIDE*SIDE*4),ec=crc32(0,expected,SIDE*SIDE*4);
        fprintf(csv,"%u,%u,%08x,%08x,%llu\n",frame,bad,ac,ec,(unsigned long long)(ms()-started));
        if(bad){failing_frames++;bad_total+=bad;unsigned char *a=actual+first*4,*e=expected+first*4;
            fprintf(stderr,"MISMATCH frame=%u bad=%u first=%u,%u expected=%u,%u,%u,%u actual=%u,%u,%u,%u\n",frame,bad,first%SIDE,first/SIDE,e[0],e[1],e[2],e[3],a[0],a[1],a[2],a[3]);
            if(saved<SAVED_LIMIT){snprintf(path,sizeof(path),"%s/frame-%06u-actual.png",output,frame);png(path,actual);snprintf(path,sizeof(path),"%s/frame-%06u-expected.png",output,frame);png(path,expected);saved++;}
        }
        if(frame%100==0||frame+1==frames){printf("frame=%u/%u failing_frames=%u bad_pixels=%llu\n",frame+1,frames,failing_frames,(unsigned long long)bad_total);fflush(stdout);fflush(csv);}
    }
    if(fclose(csv))die("close results");
    printf("SUMMARY frames=%u failing_frames=%u rate=%.8f bad_pixels=%llu saved_pairs=%u mode=%s\n",frames,failing_frames,(double)failing_frames/frames,(unsigned long long)bad_total,saved,software?"software-oracle-control":"RadeonSI");
    glDeleteTextures(SOURCES,textures);glDeleteTextures(1,&target);glDeleteFramebuffers(1,&fbo);glDeleteBuffers(1,&vbo);glDeleteVertexArrays(1,&vao);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);free(upload);free(actual);free(expected);
    eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(dpy,ctx);eglTerminate(dpy);if(gbm)gbm_device_destroy(gbm);if(fd>=0)close(fd);
    return failing_frames?1:0;
}
