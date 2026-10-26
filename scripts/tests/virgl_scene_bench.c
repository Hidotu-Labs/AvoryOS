/* Resource-heavy GL submission benchmark. No game data or settings are changed. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double seconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
int main(void) {
    Display *d = XOpenDisplay(NULL); if (!d) return 2;
    int attr[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None};
    XVisualInfo *v = glXChooseVisual(d, DefaultScreen(d), attr); if (!v) return 3;
    XSetWindowAttributes a = {.colormap=XCreateColormap(d,RootWindow(d,v->screen),v->visual,AllocNone)};
    Window w=XCreateWindow(d,RootWindow(d,v->screen),0,0,800,600,0,v->depth,InputOutput,v->visual,CWColormap,&a);
    XStoreName(d,w,"Virgl mesh submission benchmark"); XMapWindow(d,w);
    GLXContext c=glXCreateContext(d,v,NULL,True); if(!c||!glXMakeCurrent(d,w,c))return 4;
    const char *renderer=(const char *)glGetString(GL_RENDERER);
    printf("GL_RENDERER = %s\n",renderer);fflush(stdout);
    if(!renderer||!strstr(renderer,"virgl"))return 5;
    enum {N=1024}; GLuint buffers[N]; glGenBuffers(N,buffers);
    for(int i=0;i<N;i++) {
        float x=-.98f+(i%32)*.061f,y=-.98f+(i/32)*.061f;
        float vertices[]={x,y,0,x+.05f,y,0,x,y+.05f,0};
        glBindBuffer(GL_ARRAY_BUFFER,buffers[i]);
        glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);
    }
    glViewport(0,0,800,600);glEnableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_DITHER);glClearColor(.1f,.15f,.2f,1);
    double start=seconds();unsigned frames=0,samples=0;
    while(samples<2){
        glClear(GL_COLOR_BUFFER_BIT);
        for(int i=0;i<N;i++) {
            glBindBuffer(GL_ARRAY_BUFFER,buffers[i]);
            glVertexPointer(3,GL_FLOAT,0,0);
            glColor3f((i%7)/7.f,.6f,.8f);glDrawArrays(GL_TRIANGLES,0,3);
        }
        glXSwapBuffers(d,w);frames++;
        double elapsed=seconds()-start;
        if(elapsed>=5){
            glFinish();if(glGetError()!=GL_NO_ERROR)return 6;
            printf("%u frames in %.3f seconds = %.3f FPS\n",frames,elapsed,frames/elapsed);fflush(stdout);
            frames=0;start=seconds();samples++;
        }
    }
    glDeleteBuffers(N,buffers);glXMakeCurrent(d,None,NULL);glXDestroyContext(d,c);XDestroyWindow(d,w);XCloseDisplay(d);return 0;
}
