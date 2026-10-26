 for(int mode=0;mode<2;mode++) {
  const char *socket=mode?"/tmp/runtime/wayland-0":"/tmp/.X11-unix/X0";
  setenv("DISPLAY",":0",1);setenv("WAYLAND_DISPLAY","wayland-0",1);
  setenv("XDG_RUNTIME_DIR","/tmp/runtime",1);setenv("LIBSEAT_BACKEND","seatd",1);
  setenv("SEATD_VTBOUND","0",1);
  pid_t seatd=-1;if(mode){seatd=fork();if(!seatd){execl("/usr/bin/seatd","seatd","-l","debug",NULL);_exit(127);}sleep(1);}
  mkdir("/tmp/runtime",0700);
  pid_t daemon=fork();
  if(!daemon) {
   if(mode)execl("/usr/bin/weston","weston","--backend=drm","--drm-device=card0","--socket=wayland-0","--renderer=gl","--shell=kiosk-shell.so","--continue-without-input","--no-config","--log=/tmp/weston.log",NULL);
   else execl("/usr/lib/Xorg","Xorg",":0","vt1","-config","/etc/X11/xorg.conf","-logfile","/tmp/xorg.log","-noreset","-nolisten","tcp",NULL);
   _exit(127);
  }
  int ready=0,daemon_status=0;
  for(int wait=0;wait<25;wait++) {if(!access(socket,F_OK)){ready=1;break;}if(waitpid(daemon,&daemon_status,WNOHANG)==daemon)break;sleep(1);}
  int status=-1;
  if(ready) {
   pid_t client=fork();if(!client){int log=open(mode?"/tmp/egl-wayland.log":"/tmp/egl-x11.log",O_CREAT|O_WRONLY|O_TRUNC,0644);dup2(log,1);dup2(log,2);close(log);execl("/bin/virgl-egl","virgl-egl",mode?"wayland":"x11",NULL);_exit(127);}
   waitpid(client,&status,0);
  }
  int tty=open("/dev/tty1",O_RDWR);if(tty>=0){ioctl(tty,0x4b3a,0);close(tty);}
  printf("DESKTOP: %s ready=%d status=%d\n",mode?"wayland":"x11",ready,status);
  kill(daemon,SIGTERM);waitpid(daemon,&daemon_status,0);
  FILE *client_log=fopen(mode?"/tmp/egl-wayland.log":"/tmp/egl-x11.log","r");if(client_log){char line[1024];while(fgets(line,sizeof(line),client_log))fputs(line,stdout);fclose(client_log);}
  FILE *log=fopen(mode?"/tmp/weston.log":"/tmp/xorg.log","r");if(log){char line[1024];while(fgets(line,sizeof(line),log))fputs(line,stdout);fclose(log);}
  if(seatd>0){kill(seatd,SIGTERM);waitpid(seatd,NULL,0);}
  if(!ready||!WIFEXITED(status)||WEXITSTATUS(status))failures++;
 }
