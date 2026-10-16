#ifndef FS_INOTIFY_H
#define FS_INOTIFY_H
#include <stdint.h>
struct vfs_node;
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
#define IN_ALL_EVENTS 0xfff
#define IN_Q_OVERFLOW 0x4000
#define IN_IGNORED 0x8000
#define IN_ONLYDIR 0x01000000
#define IN_DONT_FOLLOW 0x02000000
#define IN_EXCL_UNLINK 0x04000000
#define IN_MASK_CREATE 0x10000000
#define IN_MASK_ADD 0x20000000
#define IN_ISDIR 0x40000000
#define IN_ONESHOT 0x80000000
void inotify_notify(struct vfs_node *node, uint32_t mask, const char *name,
                    uint32_t cookie);
uint32_t inotify_move_cookie(void);
#endif
