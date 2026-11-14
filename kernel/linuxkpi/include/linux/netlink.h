#ifndef LINUXKPI_NETLINK_OVERLAY_H
#define LINUXKPI_NETLINK_OVERLAY_H

#ifdef LINUXKPI_NET_FRONTEND
#define __LINUX_NETLINK_H
#include <uapi/linux/netlink.h>
#include <linux/types.h>
struct nla_policy;
struct netlink_ext_ack {
  const char *_msg;
  const struct nlattr *bad_attr;
  const struct nla_policy *policy;
  const struct nlattr *miss_nest;
  u16 miss_type;
  u8 cookie[8];
  u8 cookie_len;
  char _msg_buf[80];
};
#define NL_SET_ERR_MSG(extack, msg) do { \
  if ((extack) != NULL) (extack)->_msg = (msg); \
} while (0)
#else
#include_next <linux/netlink.h>
#endif

#endif
