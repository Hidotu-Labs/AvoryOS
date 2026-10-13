#!/usr/bin/env python3
"""Run the real quiet Wine environment helper, preserving explicit overrides."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
def function(file, signature):
 s=(root/file).read_text(); a=s.index(signature); b=s.index('{',a)+1; depth=1
 while depth:
  depth+=(s[b]=='{')-(s[b]=='}'); b+=1
 return s[a:b]+'\n'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fail_alloc,allocs,live;
static void *kmalloc(size_t n){
 if(++allocs==fail_alloc)return NULL;
 void *p=malloc(n);if(p)live++;return p;
}
static void kfree(void *p){if(p){live--;free(p);}}
'''
code+=function('kernel/src/syscalls/sys_process.c','static bool exec_wine_debug_env(')
code+=r"""
static char **env(const char *value){
 char **p=kmalloc(2*sizeof(*p));assert(p);p[0]=kmalloc(strlen(value)+1);
 assert(p[0]);strcpy(p[0],value);p[1]=NULL;return p;
}
static void release(char **p,int n){for(int i=0;i<n;i++)kfree(p[i]);kfree(p);}
int main(void){
 const char *paths[]={"/usr/bin/wine","wine64","/usr/lib/wine/wine-preloader","wine64-preloader"};
 for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
  int n=1;char **p=env("PATH=/bin");
  assert(exec_wine_debug_env(paths[i],&p,&n) && n==2 && p[2]==NULL);
  assert(!strcmp(p[0],"PATH=/bin") && !strcmp(p[1],"WINEDEBUG=-all"));
  char **old=p;assert(exec_wine_debug_env(paths[i],&p,&n));
  assert(p==old && n==2);release(p,n);
 }
 int n=1;char **p=env("PATH=/bin"),**old=p;
 assert(exec_wine_debug_env("/bin/bash",&p,&n) && p==old && n==1);
 assert(exec_wine_debug_env("/usr/bin/wine-tools",&p,&n) && p==old && n==1);
 release(p,n);
 const char *settings[]={"WINEDEBUG=-all","WINEDEBUG=trace+all","WINEDEBUG="};
 for(unsigned i=0;i<sizeof(settings)/sizeof(settings[0]);i++){
  n=1;p=env(settings[i]);old=p;
  assert(exec_wine_debug_env("wine",&p,&n) && p==old && n==1);
  assert(!strcmp(p[0],settings[i]));release(p,n);
 }
 for(int stage=1;stage<=2;stage++){
  n=1;p=env("PATH=/bin");old=p;fail_alloc=allocs+stage;
  assert(!exec_wine_debug_env("wine",&p,&n) && p==old && n==1);
  assert(!strcmp(p[0],"PATH=/bin") && p[1]==NULL);fail_alloc=0;release(p,n);
 }
 assert(live==0);
 puts("PASS: quiet Wine defaults, Wine-only matching, explicit overrides, inheritance and OOM rollback");
}
"""
with tempfile.TemporaryDirectory(prefix='avory-wine-debug-env-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
