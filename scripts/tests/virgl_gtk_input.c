#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Window find(Display*d,Window w){char *name=NULL;XFetchName(d,w,&name);if(name){int ok=strstr(name,"QEMU")!=NULL;XFree(name);if(ok)return w;}Window r,p,*ch;unsigned n;if(!XQueryTree(d,w,&r,&p,&ch,&n))return 0;Window found=0;for(unsigned i=0;i<n&&!found;i++)found=find(d,ch[i]);if(ch)XFree(ch);return found;}
int main(int argc,char**argv){Display*d=XOpenDisplay(NULL);if(!d)return 2;Window root=DefaultRootWindow(d),w=find(d,root);if(!w)return 3;XWindowAttributes a;XGetWindowAttributes(d,w,&a);Window child;int x,y;XTranslateCoordinates(d,w,root,0,0,&x,&y,&child);if(argc>1&&!strcmp(argv[1],"capture")){XImage*im=XGetImage(d,w,0,0,a.width,a.height,AllPlanes,ZPixmap);if(!im)return 4;FILE*f=fopen(argv[2],"wb");fprintf(f,"P6\n%d %d\n255\n",a.width,a.height);for(int j=0;j<a.height;j++)for(int i=0;i<a.width;i++){unsigned long p=XGetPixel(im,i,j);fputc((p&im->red_mask)>>16,f);fputc((p&im->green_mask)>>8,f);fputc(p&im->blue_mask,f);}fclose(f);XDestroyImage(im);printf("%d %d\n",a.width,a.height);}else{int index=argc>1?atoi(argv[1]):0;XTestFakeMotionEvent(d,-1,x+(index%2?a.width/4:3*a.width/4),y+a.height/2,CurrentTime);if(index==0||index>=30){XTestFakeButtonEvent(d,1,True,CurrentTime);XTestFakeButtonEvent(d,1,False,CurrentTime);}XSync(d,False);}XCloseDisplay(d);return 0;}
