/* Native signal/restart regression using a real LinuxKPI dma-fence wait.
 * No physical GPU, X11, driver patch or game setting is needed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include <uapi/kpi_dmabuf.h>

static volatile sig_atomic_t handled;
static volatile int64_t frame_rax;
static void caught(int sig, siginfo_t *info, void *context) {
  (void)sig; (void)info;
  ucontext_t *uc = context;
  frame_rax = (int64_t)uc->uc_mcontext.gregs[REG_RAX];
  handled++;
}
static void require(int ok, const char *message) {
  if (!ok) { fprintf(stderr,"FAIL: %s errno=%d\n",message,errno);exit(1); }
}
static void run_case(int restart, int blocked) {
  int dev=open("/dev/kpi_dmabuf",O_RDWR);
  require(dev>=0,"open native fence test device");
  int fence=ioctl(dev,KPI_DMABUF_IOC_SYNC_FD,0);
  require(fence>=0,"create unsignaled fence");
  struct sigaction sa={.sa_sigaction=caught,.sa_flags=SA_SIGINFO|(restart?SA_RESTART:0)};
  sigemptyset(&sa.sa_mask);
  require(!sigaction(SIGUSR1,&sa,NULL),"install signal handler");
  sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);
  require(!sigprocmask(blocked?SIG_BLOCK:SIG_UNBLOCK,&mask,NULL),"set signal mask");
  handled=0;frame_rax=0;
  int gate[2];require(!pipe(gate),"create child gate");
  pid_t parent=getpid(), child=fork();require(child>=0,"fork signal producer");
  if (!child) {
    char byte;close(gate[1]);
    if(read(gate[0],&byte,1)!=1)_exit(2);
    usleep(100000);
    if(kill(parent,SIGUSR1))_exit(3);
    usleep(150000);
    if(ioctl(dev,KPI_DMABUF_IOC_SIGNAL,0))_exit(4);
    _exit(0);
  }
  close(gate[0]);require(write(gate[1],"x",1)==1,"release signal producer");close(gate[1]);
  errno=0;
  int result=ioctl(dev,KPI_DMABUF_IOC_WAIT_INTR,fence), saved_errno=errno;
  if (!restart && !blocked) {
    require(result==-1 && saved_errno==EINTR,"non-restarting ioctl returns EINTR, never errno 512");
    require(handled==1 && frame_rax==-EINTR,"signal frame contains EINTR");
  } else {
    require(result==0,"restarted or blocked-signal fence wait completes");
    if (!blocked) require(handled==1 && frame_rax==16,"SA_RESTART frame restores original ioctl number");
    else require(handled==0,"blocked signal does not interrupt fence wait");
  }
  int status;require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"join signal producer");
  if (blocked) {
    require(!sigprocmask(SIG_UNBLOCK,&mask,NULL),"unblock queued signal");
    require(handled==1,"blocked signal delivered once after unblocking");
  }
  close(fence);close(dev);
  printf("PASS: real dma-fence ioctl restart=%d blocked=%d result=%d errno=%d signal-frame-rax=%lld\n",
         restart,blocked,result,saved_errno,(long long)frame_rax);
}
int main(void) {
  setvbuf(stdout,NULL,_IOLBF,0);
  run_case(0,0);run_case(1,0);run_case(1,1);
  puts("PASS: native syscall restart and signal-frame ABI");
  return 0;
}
