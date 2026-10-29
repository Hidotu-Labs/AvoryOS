#!/usr/bin/env python3
"""Exercise GNU cat's EOF handling for sched_debug under runnable load."""
from boot_probe import run_probe
from test_firefox_responsiveness_boot import prepare_desktop_disk

INIT = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <time.h>
static unsigned long long ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000ULL+t.tv_nsec/1000000;}
static void child_exited(int signal){(void)signal;}
static int run_cat(const char *destination){
 pid_t child=fork();if(!child){int fd=open(destination,O_CREAT|O_WRONLY|O_TRUNC,0600);if(fd<0)_exit(125);dup2(fd,1);close(fd);execl("/usr/bin/cat","cat","/proc/sched_debug",NULL);_exit(126);}
 int status=0;unsigned long long until=ms()+5000;
 while(ms()<until){if(waitpid(child,&status,WNOHANG)==child){printf("CAT: target=%s status=%d\n",destination,status);return status==0;}usleep(10000);}
 char path[128],buffer[2048];snprintf(path,sizeof(path),"/proc/%d/syscall",child);int fd=open(path,O_RDONLY);int n=fd<0?0:read(fd,buffer,sizeof(buffer)-1);if(n>0){buffer[n]=0;printf("CAT_STUCK: %s\n",buffer);}if(fd>=0)close(fd);
 kill(child,SIGKILL);printf("CAT: FAIL target=%s did not finish\n",destination);return 0;
}
int main(void){setbuf(stdout,NULL);struct sigaction action={.sa_handler=child_exited,.sa_flags=SA_RESTART};sigemptyset(&action.sa_mask);sigaction(SIGCHLD,&action,NULL);puts("SCHED_DEBUG_CAT_BOOT: START");mount("proc","/proc","proc",0,NULL);mount("sysfs","/sys","sysfs",0,NULL);
 pid_t load[16];for(int i=0;i<16;i++){load[i]=fork();if(!load[i]){unsigned long long until=ms()+20000;while(ms()<until){} _exit(0);}}
 int ok=run_cat("/root/sched-cat.log");ok=run_cat("/dev/kmsg")&&ok;ok=run_cat("/dev/ttyS0")&&ok;
 for(int i=0;i<16;i++)if(load[i]>0)kill(load[i],SIGKILL);
 puts(ok?"SCHED_DEBUG_CAT_BOOT: PASS":"SCHED_DEBUG_CAT_BOOT: FAIL");fflush(stdout);reboot(RB_POWER_OFF);for(;;)pause();}
'''
if __name__ == '__main__':
    run_probe('sched-debug-cat-boot', INIT, 'SCHED_DEBUG_CAT_BOOT: PASS',cpus=8,memory='1G',timeout=70,
              prepare_disk=prepare_desktop_disk,kernel_cmdline='kpi_amdgpu=0 init=/bin/firefox-probe-init')
