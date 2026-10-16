#ifndef FS_INOTIFY_LIMITS_H
#define FS_INOTIFY_LIMITS_H

/* Shared by the implementation and its read-only procfs limit files. */
#define MAX_INOTIFY_WATCHES 128
/* Desktop services and Chromium each keep multiple instances open. */
#define MAX_INOTIFY_INSTANCES 128

#endif
