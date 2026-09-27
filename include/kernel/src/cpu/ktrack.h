#ifndef CPU_KTRACK_H
#define CPU_KTRACK_H

#include <stdint.h>

// Subsystem category identifiers
#define KSUBSYS_AF_UNIX    "SOCKET/AF_UNIX"
#define KSUBSYS_AF_INET    "SOCKET/AF_INET"
#define KSUBSYS_AF_NETLINK "SOCKET/AF_NETLINK"
#define KSUBSYS_SOCKET     "SOCKET/GENERIC"
#define KSUBSYS_VFS        "FS/VFS"
#define KSUBSYS_EXT2       "FS/EXT2"
#define KSUBSYS_FAT32      "FS/FAT32"
#define KSUBSYS_RAMFS      "FS/RAMFS"
#define KSUBSYS_PROCFS     "FS/PROCFS"
#define KSUBSYS_SYSFS      "FS/SYSFS"
#define KSUBSYS_DEVFS      "FS/DEVFS"
#define KSUBSYS_MM         "MM/VMM"
#define KSUBSYS_PROCESS    "PROCESS"
#define KSUBSYS_SIGNAL     "SIGNAL"
#define KSUBSYS_FUTEX      "FUTEX"
#define KSUBSYS_EPOLL      "EPOLL"
#define KSUBSYS_POLL       "POLL"
#define KSUBSYS_SHM        "IPC/SHM"
#define KSUBSYS_SEM        "IPC/SEM"
#define KSUBSYS_DRM        "DRIVERS/DRM"
#define KSUBSYS_PTY        "DRIVERS/PTY"
#define KSUBSYS_UACCESS    "ARCH/UACCESS"
#define KSUBSYS_AUDIO      "DRIVERS/AUDIO"
#define KSUBSYS_SYSCALL    "SYSCALL"

// Forward declaration
struct thread;
struct thread *sched_get_current(void);

void ktrack_record(const char *subsys, const char *file, uint32_t line, const char *func, int64_t err);

#define KTRACK(subsys) \
    ktrack_record(subsys, __FILE__, __LINE__, __func__, 0)

#define KTRACK_ERR(subsys, err) \
    ktrack_record(subsys, __FILE__, __LINE__, __func__, (int64_t)(err))

#define KTRACK_SITE(subsys, file, line, func, err) \
    ktrack_record(subsys, file, line, func, (int64_t)(err))

#endif
