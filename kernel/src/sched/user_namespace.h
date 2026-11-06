#ifndef AVORY_USER_NAMESPACE_H
#define AVORY_USER_NAMESPACE_H
#include <stdbool.h>
#include <stdint.h>

struct thread;

/* User namespaces, enough for unprivileged sandboxes such as bubblewrap.
 *
 * A thread whose user_ns is NULL is in the initial user namespace. Any other
 * user namespace grants capabilities only through capability_has(), which
 * restricts them to what the namespace can contain (see capability.c).
 *
 * uid_map and gid_map are validated and recorded for read-back, but they are
 * not applied to credentials: inside uid 0 is not translated to the outside
 * uid. Nothing in this kernel depends on that translation yet. */

#define USER_NS_MAX_LEVEL 32
#define USER_NS_MAP_TEXT 40

/* Namespace kinds whose ownership is tracked per thread. */
enum user_ns_owned_kind {
  NS_OWN_MNT,
  NS_OWN_UTS,
  NS_OWN_IPC,
  NS_OWN_NET,
  NS_OWN_COUNT
};

struct user_namespace {
  uint64_t refs;
  struct user_namespace *parent; /* NULL: the initial user namespace. */
  uint32_t level;
  bool setgroups_denied;
  bool uid_map_set;
  bool gid_map_set;
  char uid_map[USER_NS_MAP_TEXT];
  char gid_map[USER_NS_MAP_TEXT];
};

struct user_namespace *user_namespace_create(struct user_namespace *parent);
struct user_namespace *user_namespace_get(struct user_namespace *ns);
void user_namespace_put(struct user_namespace *ns);
void user_namespace_install(struct thread *t, struct user_namespace *ns);
void user_namespace_inherit(struct thread *child, struct thread *parent);
void user_namespace_release(struct thread *t);
void user_namespace_mark_owned(struct thread *t, unsigned kind);

/* Writes to /proc/self/{uid_map,gid_map,setgroups}. Return bytes consumed or
 * a negative errno. */
int64_t user_namespace_write_map(struct thread *t, bool gid, const char *buf, uint32_t len);
int64_t user_namespace_write_setgroups(struct thread *t, const char *buf, uint32_t len);
/* Copies the recorded map text for t's user namespace into out. Returns the
 * length, or 0 when t is in the initial user namespace. */
uint32_t user_namespace_map_text(struct thread *t, bool gid, char *out, uint32_t cap);
bool user_namespace_setgroups_denied(struct thread *t);

static inline void user_namespace_cleanup(struct user_namespace **ns) {
  user_namespace_put(*ns);
}

#endif
