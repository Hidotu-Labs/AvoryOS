#include "user_namespace.h"
#include "sched.h"
#include "../mm/heap.h"
#include "../lib/string.h"

_Static_assert(NS_OWN_COUNT == 4, "thread ns_owner[] must match NS_OWN_COUNT");

struct user_namespace *user_namespace_get(struct user_namespace *ns) {
  if (ns) __atomic_add_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL);
  return ns;
}

void user_namespace_put(struct user_namespace *ns) {
  if (!ns || __atomic_sub_fetch(&ns->refs, 1, __ATOMIC_ACQ_REL)) return;
  user_namespace_put(ns->parent);
  kfree(ns);
}

/* The parent reference keeps every ancestor alive, so a thread's namespace
 * owner pointers (compared, never dereferenced) stay valid as long as its
 * user_ns does. */
struct user_namespace *user_namespace_create(struct user_namespace *parent) {
  uint32_t level = parent ? parent->level + 1 : 1;
  if (level > USER_NS_MAX_LEVEL) return NULL;
  struct user_namespace *ns = kmalloc(sizeof(*ns));
  if (!ns) return NULL;
  memset(ns, 0, sizeof(*ns));
  ns->refs = 1;
  ns->level = level;
  ns->parent = user_namespace_get(parent);
  return ns;
}

/* Takes over the caller's reference to ns. */
void user_namespace_install(struct thread *t, struct user_namespace *ns) {
  spinlock_acquire(&t->namespace_lock);
  struct user_namespace *old = t->user_ns;
  t->user_ns = ns;
  spinlock_release(&t->namespace_lock);
  user_namespace_put(old);
}

void user_namespace_inherit(struct thread *child, struct thread *parent) {
  spinlock_acquire(&parent->namespace_lock);
  child->user_ns = user_namespace_get(parent->user_ns);
  for (unsigned i = 0; i < NS_OWN_COUNT; i++)
    child->ns_owner[i] = parent->ns_owner[i];
  spinlock_release(&parent->namespace_lock);
}

void user_namespace_release(struct thread *t) { user_namespace_install(t, NULL); }

void user_namespace_mark_owned(struct thread *t, unsigned kind) {
  if (kind >= NS_OWN_COUNT) return;
  spinlock_acquire(&t->namespace_lock);
  t->ns_owner[kind] = t->user_ns;
  spinlock_release(&t->namespace_lock);
}

bool user_namespace_setgroups_denied(struct thread *t) {
  if (!t) return false;
  spinlock_acquire(&t->namespace_lock);
  bool denied = t->user_ns && t->user_ns->setgroups_denied;
  spinlock_release(&t->namespace_lock);
  return denied;
}

static bool parse_u32(const char **p, uint32_t *out) {
  uint64_t value = 0;
  unsigned digits = 0;
  while (**p >= '0' && **p <= '9') {
    value = value * 10 + (uint64_t)(**p - '0');
    if (value > 0xFFFFFFFFu) return false;
    (*p)++;
    digits++;
  }
  if (!digits) return false;
  *out = (uint32_t)value;
  return true;
}

static bool skip_blank(const char **p) {
  if (**p != ' ') return false;
  while (**p == ' ') (*p)++;
  return true;
}

/* Accepts one "<inside> <outside> <count>" line. Only a single extent is
 * supported, and an unprivileged writer may map only its own id, as Linux
 * requires. */
int64_t user_namespace_write_map(struct thread *t, bool gid, const char *buf, uint32_t len) {
  if (!t) return -1;
  spinlock_acquire(&t->namespace_lock);
  struct user_namespace *ns = t->user_ns;
  bool already_set = !ns || (gid ? ns->gid_map_set : ns->uid_map_set);
  bool needs_setgroups_deny = gid && ns && !ns->setgroups_denied;
  spinlock_release(&t->namespace_lock);
  /* The initial namespace is not writable, and each map is written once. */
  if (already_set || needs_setgroups_deny) return -1;
  if (len == 0 || len >= USER_NS_MAP_TEXT) return -22;

  char text[USER_NS_MAP_TEXT];
  memcpy(text, buf, len);
  text[len] = '\0';
  for (uint32_t i = 0; i + 1 < len; i++)
    if (text[i] == '\n') return -22;

  const char *p = text;
  uint32_t inside, outside, count;
  if (!parse_u32(&p, &inside) || !skip_blank(&p) || !parse_u32(&p, &outside) ||
      !skip_blank(&p) || !parse_u32(&p, &count))
    return -22;
  if (*p == '\n') p++;
  if (*p) return -22;
  if (count != 1) return -22;

  spinlock_acquire(&t->namespace_lock);
  uint32_t own = gid ? t->egid : t->euid;
  if (outside != own) {
    spinlock_release(&t->namespace_lock);
    return -1;
  }
  if (gid ? ns->gid_map_set : ns->uid_map_set) {
    spinlock_release(&t->namespace_lock);
    return -1;
  }
  memcpy(gid ? ns->gid_map : ns->uid_map, text, len);
  if (gid) ns->gid_map_set = true;
  else ns->uid_map_set = true;
  spinlock_release(&t->namespace_lock);
  (void)inside;
  return (int64_t)len;
}

/* "deny" is final once a gid map exists, and "allow" is refused after
 * "deny", as in Linux. */
int64_t user_namespace_write_setgroups(struct thread *t, const char *buf, uint32_t len) {
  if (!t) return -1;
  uint32_t n = len;
  if (n && buf[n - 1] == '\n') n--;
  bool deny = n == 4 && memcmp(buf, "deny", 4) == 0;
  bool allow = n == 5 && memcmp(buf, "allow", 5) == 0;
  if (!deny && !allow) return -22;

  spinlock_acquire(&t->namespace_lock);
  struct user_namespace *ns = t->user_ns;
  int64_t result = (int64_t)len;
  if (!ns) {
    result = -1;
  } else if (deny) {
    if (ns->gid_map_set && !ns->setgroups_denied) result = -1;
    else ns->setgroups_denied = true;
  } else if (ns->setgroups_denied) {
    result = -1;
  }
  spinlock_release(&t->namespace_lock);
  return result;
}

uint32_t user_namespace_map_text(struct thread *t, bool gid, char *out, uint32_t cap) {
  if (!t || !out || !cap) return 0;
  spinlock_acquire(&t->namespace_lock);
  const char *src = t->user_ns ? (gid ? t->user_ns->gid_map : t->user_ns->uid_map) : "";
  uint32_t len = 0;
  while (len + 1 < cap && len < USER_NS_MAP_TEXT && src[len]) {
    out[len] = src[len];
    len++;
  }
  spinlock_release(&t->namespace_lock);
  return len;
}
