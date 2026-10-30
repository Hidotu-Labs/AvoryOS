#!/usr/bin/env python3
"""Verify native FS_BASE and GS_BASE survive user-thread preemption."""
import subprocess

from boot_probe import run_probe


CHILD = r'''
typedef unsigned long u64;
static volatile int done;
static u64 stack[8192] __attribute__((aligned(16)));
static u64 parent_fs[2], parent_gs[2], child_fs[2], child_gs[2];

static long call6(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f) {
  register u64 r10 __asm__("r10")=d;
  register u64 r8 __asm__("r8")=e;
  register u64 r9 __asm__("r9")=f;
  long ret;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(n),"D"(a),"S"(b),"d"(c),
                   "r"(r10),"r"(r8),"r"(r9) : "rcx","r11","memory");
  return ret;
}
static long call1(u64 n,u64 a) {
  long ret;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(n),"D"(a)
                   : "rcx","r11","memory");
  return ret;
}
static u64 read_fs(void) { u64 v; __asm__ volatile("rdfsbase %0":"=r"(v)); return v; }
static u64 read_gs(void) { u64 v; __asm__ volatile("rdgsbase %0":"=r"(v)); return v; }
static void write_fs(u64 v) { __asm__ volatile("wrfsbase %0"::"r"(v):"memory"); }
static void write_gs(u64 v) { __asm__ volatile("wrgsbase %0"::"r"(v):"memory"); }
static void finish(int code) {
  const char *s = code == 0 ? "NATIVE_FSGSBASE_SWITCH: PASS\n" :
                  code == 1 ? "NATIVE_FSGSBASE_SWITCH: FAIL clone\n" :
                  code == 2 ? "NATIVE_FSGSBASE_SWITCH: FAIL child tls\n" :
                  code == 3 ? "NATIVE_FSGSBASE_SWITCH: FAIL parent tls\n" :
                              "NATIVE_FSGSBASE_SWITCH: FAIL worker timeout\n";
  unsigned long n = code == 0 ? 29 :
                    code == 1 ? 35 :
                    code == 2 ? 39 :
                    code == 3 ? 40 : 44;
  call6(1,1,(u64)s,n,0,0,0);
  call1(60,(u64)code);
  for (;;) {}
}
void _start(void) {
  write_fs((u64)parent_fs);
  write_gs((u64)parent_gs);
  const u64 flags = 0x00000100 | 0x00000200 | 0x00000400 |
                    0x00000800 | 0x00010000 | 0x00040000;
  long tid=call6(56,flags,(u64)(stack+8192),0,0,0,0);
  if (tid < 0) finish(1);
  if (tid == 0) {
    write_fs((u64)child_fs);
    write_gs((u64)child_gs);
    for (int i=0;i<2000;i++) {
      if (read_fs() != (u64)child_fs || read_gs() != (u64)child_gs)
        finish(2);
      call1(24,0);
    }
    done=1;
    call1(60,0);
    for (;;) {}
  }
  for (int i=0;i<100000 && !done;i++) {
    if (read_fs() != (u64)parent_fs || read_gs() != (u64)parent_gs)
      finish(3);
    call1(24,0);
  }
  if (!done) finish(4);
  finish(0);
}
'''

INIT = r'''
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/reboot.h>
int main(void) {
  setbuf(stdout, NULL);
  puts("NATIVE_FSGSBASE_SWITCH_BOOT: START");
  int status = 0;
  pid_t p = fork();
  if (!p) { execl("/bin/native-fsgsbase", "native-fsgsbase", NULL); _exit(127); }
  if (p < 0 || waitpid(p, &status, 0) != p || status != 0) status = 1;
  printf("NATIVE_FSGSBASE_SWITCH_BOOT: %s status=%d\n", status ? "FAIL" : "PASS", status);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}
'''


def setup(root):
    source = root.parent / 'native-fsgsbase.c'
    source.write_text(CHILD)
    subprocess.run([
        'cc', '-O2', '-fno-pie', '-fno-stack-protector', '-nostdlib', '-static',
        '-Wl,-e,_start', str(source), '-o', str(root / 'bin/native-fsgsbase')
    ], check=True)


if __name__ == '__main__':
    run_probe('native-fsgsbase-switch-boot', INIT,
              'NATIVE_FSGSBASE_SWITCH_BOOT: PASS', setup=setup, cpus=1,
              accel='tcg')
