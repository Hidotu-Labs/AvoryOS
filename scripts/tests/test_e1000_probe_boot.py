#!/usr/bin/env python3
"""Boot NIC combinations with the current kernel and private disposable disks.

Requires an existing ISO, musl toolchain, QEMU, and the syscall boot helper's
dependencies. Does not rebuild disk.img or attach any host PCI devices.
"""
from pathlib import Path
import argparse
import fcntl
import importlib.util
import shutil
import socket
import socketserver
import subprocess
import threading
import time
import json
import re

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/e1000-probe-boot'
spec = importlib.util.spec_from_file_location('boot', ROOT / 'scripts/syscall-restart-boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)

INIT = r'''
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static int errors;
static void check(int ok, const char *what);
static unsigned long long counter(const char *name) {
 char path[192];unsigned long long value=0;
 snprintf(path,sizeof(path),"/sys/class/net/@IFACE@/statistics/%s",name);
 FILE *file=fopen(path,"r");
 if(!file || fscanf(file,"%llu",&value)!=1)errors++;
 if(file)fclose(file);
 return value;
}
static int carrier(void) {
 FILE *file=fopen("/sys/class/net/@IFACE@/carrier","r");int value=-1;
 if(file){fscanf(file,"%d",&value);fclose(file);}return value;
}
static double seconds(void) {
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
 return (double)ts.tv_sec+(double)ts.tv_nsec/1000000000.;
}
static unsigned long long free_memory_kb(void) {
 FILE *file=fopen("/proc/meminfo","r");char line[256];unsigned long long value=0;
 if(file) {while(fgets(line,sizeof(line),file))if(sscanf(line,"MemFree: %llu kB",&value)==1)break;fclose(file);}
 return value;
}
static unsigned long long stream(unsigned duration,unsigned seed) {
 unsigned char sent[4096],got[4096];
 int fd=socket(AF_INET,SOCK_STREAM,0);
 if(fd<0)return 0;
 struct timeval timeout={5,0};
 setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
 setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
 struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(@PORT@)};
 inet_pton(AF_INET,"10.0.2.2",&address.sin_addr);
 if(connect(fd,(struct sockaddr*)&address,sizeof(address))){close(fd);return 0;}
 double end=seconds()+duration;unsigned long long total=0;
 do {
  for(unsigned i=0;i<sizeof(sent);i++)sent[i]=(i*37u+seed+(total/sizeof(sent)))^0xa5u;
  unsigned n=0;
  while(n<sizeof(sent)){int r=write(fd,sent+n,sizeof(sent)-n);if(r<=0)goto fail;n+=r;}
  n=0;
  while(n<sizeof(got)){int r=read(fd,got+n,sizeof(got)-n);if(r<=0)goto fail;n+=r;}
  if(memcmp(sent,got,sizeof(sent)))goto fail;
  total+=sizeof(sent);
 } while(seconds()<end);
 close(fd);return total;
fail:
 close(fd);return 0;
}
static int wait_event(int fd,int up) {
 struct pollfd p={.fd=fd,.events=POLLIN};unsigned char buf[1024];
 double end=seconds()+5;
 while(seconds()<end) {
  if(poll(&p,1,250)<=0)continue;
  int n=recv(fd,buf,sizeof(buf),0);
  if(n<32)continue;
  unsigned short type,flags;memcpy(&type,buf+4,2);memcpy(&flags,buf+6,2);
  if(type!=16 || flags)continue;
  for(unsigned pos=32;pos+4<=(unsigned)n;) {
   unsigned short len,kind;memcpy(&len,buf+pos,2);memcpy(&kind,buf+pos+2,2);
   if(len<4 || pos+len>(unsigned)n)break;
   if(kind==33 && len>=5 && buf[pos+4]==up)return 1;
   pos+=(len+3u)&~3u;
  }
 }
 return 0;
}
static void toggle_link(void) {
 unsigned long long changes=counter("link_changes");
 int fd=socket(AF_NETLINK,SOCK_RAW,0);
 struct {unsigned short family,pad;unsigned pid,groups;} addr={AF_NETLINK,0,0,1};
 check(fd>=0 && !bind(fd,(struct sockaddr*)&addr,sizeof(addr)),"link notification subscription");
 for(int up=0;up<=1;up++) {
  printf("NIC_LINK: %s_READY\n",up?"UP":"DOWN");
  double end=seconds()+5;
  while(carrier()!=up && seconds()<end)usleep(20000);
  check(carrier()==up,up?"live carrier up":"live carrier down");
  FILE *file=fopen("/sys/class/net/@IFACE@/operstate","r");char state[16]={0};
  if(file){fscanf(file,"%15s",state);fclose(file);}
  check(!strcmp(state,up?"up":"down"),"live operstate");
  check(wait_event(fd,up),up?"RTM_NEWLINK carrier up":"RTM_NEWLINK carrier down");
 }
 if(fd>=0)close(fd);
 check(stream(0,91)>0,"traffic after link toggle");
 check(counter("link_changes")>=changes+2,"link transition counter");
 check(counter("recovery_failures")==0,"no failed recoveries");
}
static void check(int ok, const char *what) {
 printf("NIC_CHECK: %s %s\n", ok ? "PASS" : "FAIL", what);
 if (!ok) errors++;
}
static int command(const char *slot) {
 char path[160]; unsigned char bytes[6];
 snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/config",slot);
 int fd=open(path,O_RDONLY); if(fd<0)return -1;
 int n=read(fd,bytes,sizeof(bytes));close(fd);
 return n==6 ? bytes[4] | bytes[5]<<8 : -1;
}
static int binding(const char *slot,const char *driver) {
 char path[160], target[160]={0};
 snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/driver",slot);
 int n=readlink(path,target,sizeof(target)-1);
 char *name=strstr(target,"/drivers/");
 return n>0 && name && !strcmp(name+9,driver);
}
static void other_controller_active(const char *slot,const char *driver,int msi) {
 int cmd=command(slot);
 check(cmd>=0 && (cmd&6)==6 && !!(cmd&0x400)==msi,
       "other Intel controller remains active during rebind");
 check(binding(slot,driver),"other Intel controller keeps its driver binding");
}
static void rebind(const char *slot,const char *driver,
                   int msi,const char *other_slot,const char *other_driver,
                   int other_msi) {
 char paths[2][160];
 snprintf(paths[0],sizeof(paths[0]),"/sys/bus/pci/drivers/%s/unbind",driver);
 snprintf(paths[1],sizeof(paths[1]),"/sys/bus/pci/drivers/%s/bind",driver);
 for(int i=0;i<2;i++) {
  int fd=open(paths[i],O_WRONLY);
  int n=fd<0 ? -1 : write(fd,slot,strlen(slot));
  if(fd>=0)close(fd);
  check(n==(int)strlen(slot),i ? "Intel NIC rebind" : "Intel NIC unbind");
  int cmd=command(slot);
  check(cmd>=0 && (i ? (cmd&6)==6 && !!(cmd&0x400)==msi :
                         !(cmd&4) && (cmd&0x400)),
        i ? "Intel NIC rebind traffic enabled" : "Intel NIC DMA and INTx disabled");
  check(binding(slot,driver)==i,"Intel NIC binding lifecycle");
  if(other_slot)other_controller_active(other_slot,other_driver,other_msi);
 }
}
static void child_exited(int signal) {(void)signal;}
int main(void) {
 struct sigaction chld={.sa_handler=child_exited,.sa_flags=SA_RESTART};
 sigemptyset(&chld.sa_mask);
 if(sigaction(SIGCHLD,&chld,NULL))return 1;
 /* This probe is PID 1: retain child statuses instead of default init
  * orphan auto-reaping between successive waits. */
 setvbuf(stdout,NULL,_IOLBF,0);
 puts("NIC_PROBE: START");
 mount("sysfs","/sys","sysfs",0,NULL);
 FILE *f=fopen("/expect","r");int intel=0,rtl=0,e1000e_mask=0,msi_mask=0;
 if(!f || fscanf(f,"%d %d %d %d",&intel,&rtl,&e1000e_mask,&msi_mask)!=4)errors++;
 char expected_mac[32]={0};
 if(!f || fscanf(f,"%31s",expected_mac)!=1)errors++;
 if(f)fclose(f);
 const char *slots[]={"0000:00:08.0","0000:00:0a.0"};
 for(int i=0;i<intel;i++) {
  const char *driver=(e1000e_mask&(1<<i))?"e1000e":"e1000";
  check(binding(slots[i],driver),"Intel NIC bound to expected driver at boot");
  int cmd=command(slots[i]);
  check(cmd>=0 && (cmd&6)==6 && !!(cmd&0x400)==!!(msi_mask&(1<<i)),
        "Intel NIC active PCI command matches selected IRQ mode");
 }
 check((access("/sys/class/net/@IFACE@",F_OK)==0)==(rtl||intel),"default NIC visible in sysfs");
 if(strcmp(expected_mac,"none")) {
  char path[160],address[32]={0};
  snprintf(path,sizeof(path),"/sys/class/net/@IFACE@/address");
  f=fopen(path,"r");if(f){fscanf(f,"%31s",address);fclose(f);}
  check(!strcmp(address,expected_mac),"selected interface resolves to expected NIC MAC");
 }
 if(rtl||intel) {
  unsigned char sent[4096],got[4096];
  for(unsigned i=0;i<sizeof(sent);i++)sent[i]=(i*37u)^0xa5u;
  int fd=socket(AF_INET,SOCK_STREAM,0);
  struct timeval timeout={5,0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(@PORT@)};
  inet_pton(AF_INET,"10.0.2.2",&addr.sin_addr);
  int ok=fd>=0 && connect(fd,(struct sockaddr*)&addr,sizeof(addr))==0;
  for(unsigned iteration=0;ok && iteration<256;iteration++) {
   sent[0]=iteration;
   unsigned n=0;
   while(ok && n<sizeof(sent)) {int r=write(fd,sent+n,sizeof(sent)-n);if(r<=0)ok=0;else n+=r;}
   n=0;
   while(ok && n<sizeof(got)) {int r=read(fd,got+n,sizeof(got)-n);if(r<=0)ok=0;else n+=r;}
   if(ok && memcmp(sent,got,sizeof(sent)))ok=0;
  }
  check(ok,"TCP 1 MiB round-trip integrity and ring wraparound");
  if(fd>=0)close(fd);
  fd=socket(AF_INET,SOCK_DGRAM,0);
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  addr.sin_port=htons(@UDP_PORT@);
  ok=fd>=0;
  for(int i=0;ok && i<64;i++) {
   int length=i%2 ? 1472 : 1;
   sent[0]=i;
   int n=sendto(fd,sent,length,0,(struct sockaddr*)&addr,sizeof(addr));
   int r=recv(fd,got,sizeof(got),0);
   if(n!=length || r!=length || memcmp(sent,got,length))ok=0;
  }
  check(ok,"UDP short and MTU-size round-trip integrity");
  if(fd>=0)close(fd);
  fd=socket(AF_INET,SOCK_RAW,1);
  unsigned char ping[64]={8,0,0,0,0x12,0x34,0,1};
  for(unsigned i=8;i<sizeof(ping);i++)ping[i]=i;
  unsigned sum=0;
  for(unsigned i=0;i<sizeof(ping);i+=2)sum+=(unsigned)ping[i]<<8|ping[i+1];
  while(sum>>16)sum=(sum&0xffff)+(sum>>16);
  ping[2]=(~sum)>>8;ping[3]=~sum;
  addr.sin_port=0;
  int n=fd<0?-1:sendto(fd,ping,sizeof(ping),0,(struct sockaddr*)&addr,sizeof(addr));
  struct pollfd pollfd={.fd=fd,.events=POLLIN};
  int r=n==(int)sizeof(ping) && poll(&pollfd,1,5000)>0 ? recv(fd,got,sizeof(got),0) : -1;
  unsigned header=r>0?(got[0]&15)*4:0;
  check(r>=(int)(header+sizeof(ping)) && got[header]==0 &&
        !memcmp(got+header+4,ping+4,sizeof(ping)-4),"ICMP echo reply integrity");
  if(fd>=0)close(fd);
  fd=socket(AF_INET6,SOCK_DGRAM,0);
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  struct sockaddr_in6 addr6={.sin6_family=AF_INET6,.sin6_port=htons(@UDP6_PORT@)};
  inet_pton(AF_INET6,"2001:db8:1::2",&addr6.sin6_addr);
  n=fd<0?-1:sendto(fd,sent,64,0,(struct sockaddr*)&addr6,sizeof(addr6));
  r=n==64?recv(fd,got,sizeof(got),0):-1;
  check(r==64 && !memcmp(sent,got,64),"IPv6 UDP integrity through NDP/SLAAC");
  if(fd>=0)close(fd);
  mount("proc","/proc","proc",0,NULL);
  unsigned long long before_rx=counter("rx_bytes"),before_tx=counter("tx_bytes");
  if(@SECONDS@) {
   pid_t children[4];
   for(unsigned i=0;i<4;i++) {
    children[i]=fork();
    if(!children[i]) {
     unsigned long long bytes=stream(@SECONDS@,i+1);
     printf("NIC_LOAD: child=%u bytes=%llu\n",i,bytes);
     _exit(bytes?0:1);
    }
   }
   if(@LINKTEST@) {
    usleep(100000);
    toggle_link(); /* flap the cable while all four streams are still active */
   }
   int ok=1;
   for(unsigned i=0;i<4;i++) {
    int status=0;pid_t waited;
    do { waited=waitpid(children[i],&status,0); } while(waited<0 && errno==EINTR);
    if(children[i]<=0 || waited!=children[i] || !WIFEXITED(status) || WEXITSTATUS(status)) {
     printf("NIC_WAIT: child=%u pid=%d result=%d status=%d errno=%d\n",i,children[i],waited,status,errno);ok=0;
    }
   }
   check(ok,"sustained four-stream TCP integrity");
   check(counter("rx_bytes")>before_rx && counter("tx_bytes")>before_tx,"live RX/TX statistics grow");
  }
  if(@LINKTEST@ && !@SECONDS@)toggle_link();
  f=fopen("/proc/net/dev","r");
  if(f) {char line[512];while(fgets(line,sizeof(line),f))printf("NIC_STATS: %s",line);fclose(f);}
 }
 unsigned long long memory_before=free_memory_kb();
 for(int cycle=0;cycle<3;cycle++) {
  for(int i=0;i<intel;i++)
   rebind(slots[i],(e1000e_mask&(1<<i))?"e1000e":"e1000",
          !!(msi_mask&(1<<i)),
          intel>1?slots[1-i]:NULL,
          intel>1 && (e1000e_mask&(1<<(1-i)))?"e1000e":"e1000",
          intel>1 && !!(msi_mask&(1<<(1-i))));
  if(intel) {
   check(stream(0,cycle+42)>0,"traffic after rebind");
   if(@LINKTEST@)check(counter("dma_bytes")==1056768,"DMA footprint stable after rebind");
  }
 }
 if(intel) {
  usleep(250000);
  unsigned long long memory_after=free_memory_kb();
  printf("NIC_MEMORY: before=%llu after=%llu kB\n",memory_before,memory_after);
  check(memory_before && memory_after && memory_after+2048>=memory_before,"bounded memory across rebind cycles");
 }
 printf("NIC_PROBE: END errors=%d\n",errors);
 fflush(NULL);reboot(RB_POWER_OFF);for(;;)sleep(1);
}
'''

class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while data := self.request.recv(8192):
            self.request.sendall(data)


class UdpEcho(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        sock.sendto(data, self.client_address)


class Udp6Server(socketserver.ThreadingUDPServer):
    address_family = socket.AF_INET6


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=10, help='seconds of four-stream load per networked boot')
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--smp', type=int, choices=(1, 4, 8), default=4)
    parser.add_argument('--case', action='append', choices=('none', 'e1000', 'e1000e', 'e1000e-intx', 'e1000e-msi', 'e1000e-pair', 'rtl8139', 'mixed', 'mixed-e1000', 'mixed-intel', 'all-three', 'recovery', 'recovery-e1000e'))
    args = parser.parse_args()
    if not 0 <= args.seconds <= 300:
        parser.error('--seconds must be between 0 and 300')
    if args.accel != 'kvm':
        OUT = OUT / args.accel
    if args.smp != 4:
        OUT = OUT / f'smp{args.smp}'
    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Echo) as server, \
                socketserver.ThreadingUDPServer(('127.0.0.1', 0), UdpEcho) as udp, \
                Udp6Server(('::1', 0), UdpEcho) as udp6:
            server.daemon_threads = True
            threading.Thread(target=server.serve_forever, daemon=True).start()
            threading.Thread(target=udp.serve_forever, daemon=True).start()
            threading.Thread(target=udp6.serve_forever, daemon=True).start()
            try:
                run_cases(server.server_address[1], udp.server_address[1], udp6.server_address[1], args)
            finally:
                server.shutdown()
                udp.shutdown()
                udp6.shutdown()


def qmp_command(stream, command, arguments=None):
    stream.write((json.dumps({'execute': command, 'arguments': arguments or {}}) + '\n').encode())
    stream.flush()
    while True:
        response = json.loads(stream.readline())
        if 'error' in response:
            raise RuntimeError(response)
        if 'return' in response:
            return response['return']


def run_cases(port, udp_port, udp6_port, args):
    original_run = boot.run
    # All cases must exercise exactly the same kernel, even if another build
    # runs while the guest matrix is in progress.
    snapshot = OUT / 'kernel'
    shutil.copy2(ROOT / 'kernel/bin-x86_64/kernel', snapshot)
    subprocess.run(['strip', '--strip-all', snapshot], check=True)

    def run_with_kernel(args):
        if str(args[0]) == 'xorriso' and '-as' in args:
            target = boot.OUT / 'iso-root/boot/kernel'
            shutil.copy2(snapshot, target)
        original_run(args)

    boot.run = run_with_kernel
    # Reuse the same traffic and lifecycle checks for either Intel profile.
    cases = [
        ('none', [], 0, 'eth0', False, 'auto', 'none'),
        ('e1000', ['e1000'], 0, 'eth0', False, 'auto', '52:54:00:12:34:00'),
        ('e1000e', ['e1000e'], 0, 'eth0', True, 'auto', '52:54:00:12:34:00'),
        ('e1000e-intx', ['e1000e'], 0, 'eth0', True, 'intx', '52:54:00:12:34:00'),
        ('e1000e-msi', ['e1000e'], 0, 'eth0', True, 'msi', '52:54:00:12:34:00'),
        ('e1000e-pair', ['e1000e', 'e1000e'], 0, 'eth1', True, 'auto', '52:54:00:12:34:00'),
        ('rtl8139', [], 1, 'eth0', False, 'auto', 'none'),
        ('mixed', ['e1000', 'e1000'], 1, 'eth0', False, 'auto', 'none'),
        ('mixed-e1000', ['e1000', 'e1000'], 1, 'eth1', True, 'auto', 'none'),
        ('mixed-intel', ['e1000', 'e1000e'], 0, 'eth1', True, 'auto', '52:54:00:12:34:01'),
        ('all-three', ['e1000', 'e1000e'], 1, 'eth2', True, 'auto', '52:54:00:12:34:01'),
        ('recovery', ['e1000'], 0, 'eth0', False, 'auto', '52:54:00:12:34:00'),
        ('recovery-e1000e', ['e1000e'], 0, 'eth0', True, 'auto', '52:54:00:12:34:00'),
    ]
    for name, models, rtl, iface, link_test, irq_policy, default_mac in cases:
        if args.case and name not in args.case:
            continue
        intel = len(models)
        e1000e_mask = sum(1 << i for i, model in enumerate(models) if model == 'e1000e')
        msi_mask = e1000e_mask if irq_policy in ('auto', 'msi') else 0
        boot.INIT = INIT.replace('@PORT@', str(port)).replace('@UDP_PORT@', str(udp_port)).replace('@UDP6_PORT@', str(udp6_port)).replace('@IFACE@', iface).replace('@SECONDS@', str(args.seconds)).replace('@LINKTEST@', str(int(link_test)))
        boot.OUT = OUT / name
        boot.OUT.mkdir(exist_ok=True)
        def setup(root):
            (root / 'expect').write_text(f'{intel} {rtl} {e1000e_mask} {msi_mask} {default_mac}\n')
        fault = ' e1000_test_tx_stall=1' if name in ('recovery', 'recovery-e1000e') else ''
        disk, iso = boot.prepare(setup, kernel_cmdline=f'kpi_amdgpu=0 init=/bin/probe-init net_default={iface} e1000e_irq={irq_policy}{fault}')
        serial = boot.OUT / 'serial.log'
        qmp_path = boot.OUT / 'qmp.sock'
        qmp_path.unlink(missing_ok=True)
        cmd = ['qemu-system-x86_64', '-M', 'q35', '-cpu', 'host' if args.accel == 'kvm' else 'max', '-accel', args.accel,
               '-smp', str(args.smp), '-m', '1G', '-drive',
               f'if=pflash,unit=0,format=raw,file={ROOT}/edk2-ovmf/ovmf-code-x86_64.fd,readonly=on',
               '-cdrom', str(iso), '-boot', 'd', '-drive',
               f'file={disk},format=raw,if=none,id=probe', '-device',
               'nvme,serial=e1000-probe,drive=probe', '-nic', 'none',
               '-vga', 'none', '-device', 'virtio-vga,addr=06.0',
               '-display', 'none', '-serial', f'file:{serial}',
               '-monitor', 'none', '-qmp', f'unix:{qmp_path},server=on,wait=off', '-no-reboot']
        for i, model in enumerate(models):
            slot = (8, 10)[i]
            cmd += ['-device', f'{model},id=e{i},netdev=i{i},addr={slot:02x}.0,mac=52:54:00:12:34:{i:02x}',
                    '-netdev', f'user,id=i{i},ipv6-net=2001:db8:1::/64']
        if rtl:
            cmd += ['-device', 'rtl8139,netdev=r0,addr=09.0', '-netdev', 'user,id=r0,ipv6-net=2001:db8:1::/64']
        with (boot.OUT / 'host.log').open('wb') as output:
            process = subprocess.Popen(cmd, stdout=output, stderr=output)
            try:
                deadline = time.monotonic() + 90 + args.seconds
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.settimeout(5)
                    while not qmp_path.exists() and process.poll() is None:
                        if time.monotonic() >= deadline:
                            raise TimeoutError(str(qmp_path))
                        time.sleep(.05)
                    client.connect(str(qmp_path))
                    with client.makefile('rwb') as stream:
                        json.loads(stream.readline())
                        qmp_command(stream, 'qmp_capabilities')
                        down = up = False
                        while process.poll() is None:
                            if time.monotonic() >= deadline:
                                raise TimeoutError(str(serial))
                            log = serial.read_text(errors='replace') if serial.exists() else ''
                            device = ('e0' if name == 'e1000e-pair'
                                      else f'e{1 if iface in ("eth1", "eth2") else 0}')
                            if link_test and not down and 'NIC_LINK: DOWN_READY' in log:
                                qmp_command(stream, 'set_link', {'name': device, 'up': False})
                                down = True
                            if link_test and not up and 'NIC_LINK: UP_READY' in log:
                                qmp_command(stream, 'set_link', {'name': device, 'up': True})
                                up = True
                            time.sleep(.05)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
        log = serial.read_text(errors='replace')
        assert process.returncode == 0 and 'NIC_PROBE: END errors=0' in log, str(serial)
        for i in range(intel):
            assert f'MAC 52:54:00:12:34:{i:02x}' in log, str(serial)
            if e1000e_mask & (1 << i):
                mode = 'MSI vector' if msi_mask & (1 << i) else 'INTx IRQ'
                assert re.search(rf'E1000E] eth\d+ RX/TX enabled; {re.escape(mode)}', log), str(serial)
        if rtl or intel:
            assert log.count('[NET] Ethernet, ARP and IPv4 initialized') == 1
            assert '[DHCP] lease acquisition failed' not in log
            assert '[DHCP] Lease obtained' in log
            assert f'[NET] default interface: {iface}' in log
            if name in ('recovery', 'recovery-e1000e'):
                assert log.count('TX recovery complete') == 1, str(serial)
            else:
                assert 'TX stalled:' not in log, str(serial)
        else:
            assert '[NET] no usable network device; protocol startup skipped' in log
            assert '[NET] starting DHCP client' not in log
        if intel:
            assert log.count('shutdown: DMA/interrupts disabled, buffers=released') == intel, str(serial)
        assert 'DMA buffers quarantined' not in log, str(serial)
        print(f'PASS: {name}, SMP={args.smp}: functional traffic, {args.seconds}s load, carrier/events, watchdog, rebind and shutdown', flush=True)


if __name__ == '__main__':
    main()
