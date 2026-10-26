#!/usr/bin/env python3
"""Identity probing must work while another client owns DRM master."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
code = r'''
#include <assert.h>
#include <stdarg.h>
#define main identify_main
#define open probe_open
#define close probe_close
#define ioctl probe_ioctl
#define realpath probe_realpath
#include "drm_identify.c"
#undef main
int probe_open(const char *path, int flags, ...) {
 assert(flags == (O_RDWR | O_CLOEXEC));
 if (!strcmp(path,"/dev/dri/card0")) return 3;
 if (!strcmp(path,"/dev/dri/renderD128")) return 4;
 errno=ENOENT; return -1;
}
int probe_close(int fd) { assert(fd==3 || fd==4); return 0; }
int probe_ioctl(int fd, unsigned long request, ...) {
 assert(fd==3 || fd==4);
 va_list args;va_start(args,request);void *data=va_arg(args,void*);va_end(args);
 if (request==DRM_IOCTL_VERSION) {
  struct drm_version *v=data;memcpy(v->name,"nouveau",8);v->name_len=7;return 0;
 }
 if (request==DRM_IOCTL_GET_UNIQUE) {
  ((struct drm_unique*)data)->unique_len=0;return 0;
 }
 if (request==DRM_IOCTL_MODE_GETRESOURCES) {
  ((struct drm_mode_card_res*)data)->count_connectors=0;return 0;
 }
 assert(!"identity probe attempted a state-changing/unexpected DRM ioctl");return -1;
}
char *probe_realpath(const char *path, char *out) {
 assert(!strcmp(path,"/sys/class/drm/card0/device") ||
        !strcmp(path,"/sys/class/drm/renderD128/device"));
 strcpy(out,"/sys/bus/pci/devices/0000:01:00.0");return out;
}
int main(void) { char *args[]={"drm-identify",NULL};return identify_main(1,args); }
'''
with tempfile.TemporaryDirectory(prefix='drm-readonly-') as temp:
    path=Path(temp);(path/'probe.c').write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I',str(root/'userland'),
                    '-I',str(root/'kernel/linux/include/uapi/drm'),str(path/'probe.c'),'-o',str(path/'probe')],check=True)
    result=subprocess.run([str(path/'probe')],capture_output=True,text=True,check=True)
    assert result.stdout=='/dev/dri/card0 nouveau pci:0000:01:00.0 0 /dev/dri/renderD128\n',result.stdout
print('PASS: read-only DRM identity and sysfs card/render pairing without master-only ioctls')
