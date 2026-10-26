#ifndef AVORY_FUSE_PROTOCOL_H
#define AVORY_FUSE_PROTOCOL_H
/* Linux FUSE wire ABI, protocol 7.9 through 7.26. No Linux internal headers. */
#include <stdint.h>
#define AFUSE_LOOKUP 1
#define AFUSE_FORGET 2
#define AFUSE_GETATTR 3
#define AFUSE_READLINK 5
#define AFUSE_OPEN 14
#define AFUSE_READ 15
#define AFUSE_STATFS 17
#define AFUSE_RELEASE 18
#define AFUSE_INIT 26
#define AFUSE_OPENDIR 27
#define AFUSE_READDIR 28
#define AFUSE_RELEASEDIR 29
#define AFUSE_DESTROY 38
struct afuse_in_header {
  uint32_t len, opcode;
  uint64_t unique, nodeid;
  uint32_t uid, gid, pid, padding;
};
struct afuse_out_header { uint32_t len; int32_t error; uint64_t unique; };
struct afuse_init_in { uint32_t major, minor, max_readahead, flags; };
struct afuse_init_out {
  uint32_t major, minor, max_readahead, flags;
  uint16_t max_background, congestion_threshold;
  uint32_t max_write, time_gran;
  uint16_t max_pages, map_alignment;
  uint32_t flags2, unused[7];
};
struct afuse_attr {
  uint64_t ino, size, blocks, atime, mtime, ctime;
  uint32_t atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev,
           blksize, flags;
};
struct afuse_entry_out {
  uint64_t nodeid, generation, entry_valid, attr_valid;
  uint32_t entry_valid_nsec, attr_valid_nsec;
  struct afuse_attr attr;
};
struct afuse_attr_out {
  uint64_t attr_valid; uint32_t attr_valid_nsec, dummy;
  struct afuse_attr attr;
};
struct afuse_getattr_in { uint32_t flags, dummy; uint64_t fh; };
struct afuse_open_in { uint32_t flags, open_flags; };
struct afuse_open_out { uint64_t fh; uint32_t open_flags, padding; };
struct afuse_read_in {
  uint64_t fh, offset; uint32_t size, read_flags;
  uint64_t lock_owner; uint32_t flags, padding;
};
struct afuse_release_in {
  uint64_t fh; uint32_t flags, release_flags; uint64_t lock_owner;
};
struct afuse_forget_in { uint64_t nlookup; };
struct afuse_kstatfs {
  uint64_t blocks, bfree, bavail, files, ffree;
  uint32_t bsize, namelen, frsize, padding, spare[6];
};
struct afuse_dirent {
  uint64_t ino, off; uint32_t namelen, type; char name[];
};
_Static_assert(sizeof(struct afuse_in_header) == 40, "FUSE request ABI");
_Static_assert(sizeof(struct afuse_out_header) == 16, "FUSE response ABI");
_Static_assert(sizeof(struct afuse_attr) == 88, "FUSE attribute ABI");
#endif
