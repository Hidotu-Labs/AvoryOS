#ifndef AVORY_CAPABILITY_H
#define AVORY_CAPABILITY_H
#include <stdbool.h>
#include <stdint.h>
struct thread;
#define CAP_CHOWN 0
#define CAP_DAC_OVERRIDE 1
#define CAP_DAC_READ_SEARCH 2
#define CAP_FOWNER 3
#define CAP_KILL 5
#define CAP_KILL 5
#define CAP_SETGID 6
#define CAP_SETUID 7
#define CAP_SETPCAP 8
#define CAP_NET_ADMIN 12
#define CAP_NET_RAW 13
#define CAP_SYS_CHROOT 18
#define CAP_SYS_PTRACE 19
#define CAP_SYS_ADMIN 21
#define CAP_SYS_BOOT 22
#define CAP_SYS_NICE 23
#define CAP_LAST_CAP 40
#define CAP_VALID_MASK ((1ULL << 41) - 1)
bool capability_has(struct thread *, unsigned);
void capability_inherit(struct thread *, struct thread *);
void capability_uid_changed(struct thread *, uint32_t, uint32_t, uint32_t);
void capability_exec(struct thread *, uint32_t);
int64_t capability_prctl(struct thread *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t capability_get(uint64_t, uint64_t);
int64_t capability_set(uint64_t, uint64_t);
#endif
