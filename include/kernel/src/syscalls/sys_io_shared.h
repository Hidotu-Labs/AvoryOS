#ifndef SYS_IO_SHARED_H
#define SYS_IO_SHARED_H

// Shared types, constants, and helpers used across the sys_io modules.

#include "../fs/vfs.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "arch/uaccess.h"
#include "syscall.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Open / file-status flags
// ---------------------------------------------------------------------------
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_TRUNC 0x200
#define O_ACCMODE 3
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC 0x80000

/* O_PATH shares its bit with __O_TMPFILE, exactly like Linux (both 010000000).
 * A request is an O_TMPFILE only when the directory bit is set with it *and*
 * the caller asked for write access; read-only + O_DIRECTORY is an O_PATH
 * handle, which is what QProcess opens for a child's working directory. */
#define __O_TMPFILE 0x400000
#define O_PATH 0x400000
#define O_TMPFILE (__O_TMPFILE | O_DIRECTORY)

/* FD_CLOEXEC packed into fd_flags[] above the status-flag range */
#define FD_FLAGS_CLOEXEC_BIT (1u << 24)

/* Placeholder a descriptor slot carries between reserving the number and
 * installing the node, so a concurrent fork or lookup never sees a half-built
 * descriptor that looks like a real file. */
#define FD_RESERVED ((vfs_node_t *)-1)

#define FD_CLOEXEC 1

#define AT_FDCWD (-100)

// ---------------------------------------------------------------------------
// fcntl commands
// ---------------------------------------------------------------------------
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_SETOWN 8
#define F_DUPFD_CLOEXEC 1030

// ---------------------------------------------------------------------------
// stat / statx structures
// ---------------------------------------------------------------------------
struct kstat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint64_t st_nlink;
  uint32_t st_mode;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t __pad0;
  uint64_t st_rdev;
  int64_t st_size;
  int64_t st_blksize;
  int64_t st_blocks;
  int64_t st_atim_sec;
  int64_t st_atim_nsec;
  int64_t st_mtim_sec;
  int64_t st_mtim_nsec;
  int64_t st_ctim_sec;
  int64_t st_ctim_nsec;
  int64_t __unused[3];
};

struct statx_timestamp {
  int64_t tv_sec;
  uint32_t tv_nsec;
  int32_t __reserved;
};

struct statx {
  uint32_t stx_mask;
  uint32_t stx_blksize;
  uint64_t stx_attributes;
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode;
  uint16_t __spare0[1];
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks;
  uint64_t stx_attributes_mask;
  struct statx_timestamp stx_atime;
  struct statx_timestamp stx_btime;
  struct statx_timestamp stx_ctime;
  struct statx_timestamp stx_mtime;
  uint32_t stx_rdev_major;
  uint32_t stx_rdev_minor;
  uint32_t stx_dev_major;
  uint32_t stx_dev_minor;
  uint64_t __spare2[14];
};

#define STATX_TYPE 0x0001U
#define STATX_MODE 0x0002U
#define STATX_NLINK 0x0004U
#define STATX_UID 0x0008U
#define STATX_GID 0x0010U
#define STATX_ATIME 0x0020U
#define STATX_MTIME 0x0040U
#define STATX_CTIME 0x0080U
#define STATX_INO 0x0100U
#define STATX_SIZE 0x0200U
#define STATX_BLOCKS 0x0400U
#define STATX_BASIC_STATS 0x07ffU
#define STATX_BTIME 0x0800U

#define AT_STATX_SYNC_AS_STAT 0x0000
#define AT_STATX_FORCE_SYNC 0x2000
#define AT_STATX_DONT_SYNC 0x4000
#define AT_STATX_SYNC_TYPE 0x6000
#define AT_EMPTY_PATH 0x1000
#define AT_SYMLINK_NOFOLLOW 0x0100

// ---------------------------------------------------------------------------
// VT / KD / TTY ioctl constants
// ---------------------------------------------------------------------------
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGETD 0x5424
#define TIOCSETD 0x5423

#define VT_OPENQRY 0x5600
#define VT_GETMODE 0x5601
#define VT_SETMODE 0x5602
#define VT_GETSTATE 0x5603
#define VT_RELDISP 0x5605
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608

#define KDSETMODE 0x4B3A
#define KDGETMODE 0x4B3B
#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define KD_TEXT 0x00
#define KD_GRAPHICS 0x01

struct vt_mode {
  char mode;
  char waitv;
  short relsig;
  short acqsig;
  short frsig;
};

struct vt_stat {
  unsigned short v_active;
  unsigned short v_signal;
  unsigned short v_state;
};

// OSS /dev/dsp ioctls (numbers match musl sys/soundcard.h on x86_64)
#define SNDCTL_DSP_RESET 0x00005000
#define SNDCTL_DSP_SYNC 0x00005001
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define SNDCTL_DSP_SETFRAGMENT 0xC004500A
#define SNDCTL_DSP_GETFMTS 0x8004500B
#define SNDCTL_DSP_SETTRIGGER 0x40045010
#define SNDCTL_DSP_GETTRIGGER 0x80045010
#define SNDCTL_DSP_GETIPTR 0x800C5011
#define SNDCTL_DSP_GETOPTR 0x800C5012
#define SNDCTL_DSP_GETOSPACE 0x8010500C
#define SNDCTL_DSP_GETCAPS 0x8004500F
#define AFMT_U8 0x00000008
#define AFMT_S16_LE 0x00000010
#define PCM_ENABLE_OUTPUT 0x00000002
#define PCM_ENABLE_INPUT 0x00000001
#define DSP_CAP_TRIGGER 0x00001000
#define DSP_CAP_OUTPUT 0x00000002

struct oss_count_info {
  int bytes;
  int blocks;
  int ptr;
};

// ---------------------------------------------------------------------------
// inotify
// ---------------------------------------------------------------------------
#define MAX_INOTIFY_WATCHES 128
#define MAX_INOTIFY_INSTANCES 32

#define IN_ACCESS 0x1
#define IN_MODIFY 0x2
#define IN_ATTRIB 0x4
#define IN_CLOSE_WRITE 0x8
#define IN_CLOSE_NOWRITE 0x10
#define IN_OPEN 0x20
#define IN_MOVED_FROM 0x40
#define IN_MOVED_TO 0x80
#define IN_CREATE 0x100
#define IN_DELETE 0x200
#define IN_DELETE_SELF 0x400
#define IN_MOVE_SELF 0x800

typedef struct {
  uint32_t wd;
  char path[256];
  uint32_t mask;
  uint32_t event_mask;
} inotify_watch_t;

typedef struct {
  uint32_t instance_id;
  inotify_watch_t watches[MAX_INOTIFY_WATCHES];
  uint32_t num_watches;
  uint64_t event_queue[256];
  uint32_t queue_head;
  uint32_t queue_tail;
} inotify_instance_t;

extern inotify_instance_t inotify_instances[MAX_INOTIFY_INSTANCES];
extern uint32_t next_instance_id;
extern uint32_t next_watch_id;

// ---------------------------------------------------------------------------
// iovec (shared by readv/writev)
// ---------------------------------------------------------------------------
struct user_iovec {
  uint64_t iov_base;
  uint64_t iov_len;
};

// ---------------------------------------------------------------------------
// console termios (defined in sys_ioctl.c, used in sys_fd.c registration)
// ---------------------------------------------------------------------------
#include "../drivers/pty.h"
#include "../drivers/gpu/drm/drm.h"
#include "../fb/terminal.h"
extern struct termios console_termios;

// ---------------------------------------------------------------------------
// FD helpers (defined in sys_fd.c, used everywhere)
// ---------------------------------------------------------------------------
int alloc_fd(struct thread *t);
int alloc_fd_from(struct thread *t, int from);
uint64_t sys_open_path(int dirfd, const char *path, uint64_t flags, uint64_t mode);

// stat helper (defined in sys_stat.c, used in sys_fs.c)
void fill_kstat(struct kstat *ks, vfs_node_t *node);

// vfs_resolve_symlink_node: resolve WITHOUT following the final symlink.
// Defined in sys_fs.c; used by sys_stat.c and sys_fs.c.
// Returns an owned reference, or NULL.
vfs_node_t *vfs_resolve_symlink_node(vfs_node_t *base, const char *path);

// resolve_parent_and_name helper (defined in sys_fs.c).
// Returns an owned reference to the parent directory (caller must vfs_close),
// or NULL.
vfs_node_t *resolve_parent_and_name(const char *path, char *name_out,
                                    size_t name_size);

// timerfd tick (called from lapic_timer) — defined in sys_aio.c
void timerfd_tick(void);

#endif /* SYS_IO_SHARED_H */
