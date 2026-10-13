/* Realtime/monotonic absolute and relative futex timeout regression. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static uint32_t word;
static long ms(void) { struct timespec t; syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t); return t.tv_sec*1000L+t.tv_nsec/1000000; }
static int check(int clock, int absolute, int realtime) {
    struct timespec t={.tv_nsec=30000000};
    if (absolute) {
        clock_gettime(clock,&t); t.tv_nsec+=30000000;
        if(t.tv_nsec>=1000000000) {t.tv_sec++;t.tv_nsec-=1000000000;}
    }
    struct timespec kernel_now; syscall(SYS_clock_gettime,clock,&kernel_now);
    printf("FUTEXCLOCK: target=%ld.%09ld kernel_now=%ld.%09ld\n",t.tv_sec,t.tv_nsec,kernel_now.tv_sec,kernel_now.tv_nsec);
    long begin=ms();
    long r=syscall(SYS_futex,&word,128|(absolute?9:0)|(realtime?256:0),0,&t,0,~0u);
    long elapsed=ms()-begin;
    if(r!=-1 || errno!=ETIMEDOUT || elapsed<20 || elapsed>2000) {
        printf("FUTEXCLOCK: FAIL clock=%d absolute=%d r=%ld errno=%d elapsed=%ld\n",clock,absolute,r,errno,elapsed); return 1;
    }
    printf("FUTEXCLOCK: timeout clock=%d absolute=%d elapsed=%ldms\n",clock,absolute,elapsed);
    return 0;
}
static void *wake_waiter(void *arg) {
    (void)arg;
    struct timespec delay={.tv_nsec=20000000}; nanosleep(&delay,0);
    syscall(SYS_futex,&word,128|1,1,0,0,0);
    return 0;
}
int main(void) {
    int log=open("/dev/kmsg",O_WRONLY); if(log>=0){dup2(log,1);dup2(log,2);close(log);}
    puts("FUTEXCLOCK: START");
    if(check(CLOCK_REALTIME,1,1)||check(CLOCK_MONOTONIC,1,0)||check(CLOCK_MONOTONIC,0,0)) return 1;
    pthread_t waker;
    struct timespec future; clock_gettime(CLOCK_REALTIME,&future); future.tv_sec+=2;
    if(pthread_create(&waker,0,wake_waiter,0)) {puts("FUTEXCLOCK: FAIL pthread_create");return 1;}
    long wake_result=syscall(SYS_futex,&word,128|9|256,0,&future,0,~0u);
    pthread_join(waker,0);
    if(wake_result!=0) {puts("FUTEXCLOCK: FAIL matching wake");return 1;}
    puts("FUTEXCLOCK: matching wake passed");
    struct timespec invalid={.tv_nsec=1000000000};
    if(syscall(SYS_futex,&word,128|9,0,&invalid,0,~0u)!=-1 || errno!=EINVAL) {puts("FUTEXCLOCK: FAIL invalid timespec");return 1;}
    struct timespec past={0};
    if(syscall(SYS_futex,&word,128|9|256,0,&past,0,~0u)!=-1 || errno!=ETIMEDOUT) {puts("FUTEXCLOCK: FAIL past deadline");return 1;}
    puts("FUTEXCLOCK: PASS");
    return 0;
}
