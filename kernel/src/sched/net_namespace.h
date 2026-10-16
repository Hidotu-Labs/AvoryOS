#ifndef AVORY_NET_NAMESPACE_H
#define AVORY_NET_NAMESPACE_H
#include <stdint.h>
struct thread;
struct net_namespace {
  uint64_t refs;
  uint32_t id;
  uint32_t loopback_address;
  uint32_t loopback_flags;
};
struct net_namespace *net_namespace_create(void);
struct net_namespace *net_namespace_get(struct net_namespace *);
void net_namespace_put(struct net_namespace *);
void net_namespace_install(struct thread *, struct net_namespace *);
void net_namespace_inherit(struct thread *, struct thread *);
void net_namespace_release(struct thread *);
uint32_t net_namespace_id(struct thread *);
static inline void net_namespace_cleanup(struct net_namespace **n) { net_namespace_put(*n); }
#endif
