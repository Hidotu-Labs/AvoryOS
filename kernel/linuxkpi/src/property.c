/* Firmware-node property accessors from upstream drivers/base/property.c.
 * Dispatch goes through the node's fwnode_operations exactly as Linux does;
 * AvoryOS PCI devices currently carry no firmware node, so lookups on them
 * report absent properties and no children. */
#include <linux/err.h>
#include <linux/fwnode.h>
#include <linux/property.h>

bool fwnode_property_present(const struct fwnode_handle *fwnode,
                             const char *propname) {
  bool ret;

  if (IS_ERR_OR_NULL(fwnode))
    return false;

  ret = fwnode_call_bool_op(fwnode, property_present, propname);
  if (ret)
    return ret;

  return fwnode_call_bool_op(fwnode->secondary, property_present, propname);
}

bool fwnode_property_read_bool(const struct fwnode_handle *fwnode,
                               const char *propname) {
  bool ret;

  if (IS_ERR_OR_NULL(fwnode))
    return false;

  ret = fwnode_call_bool_op(fwnode, property_read_bool, propname);
  if (ret)
    return ret;

  return fwnode_call_bool_op(fwnode->secondary, property_read_bool, propname);
}

bool device_property_present(const struct device *dev, const char *propname) {
  return fwnode_property_present(dev_fwnode(dev), propname);
}

bool device_property_read_bool(const struct device *dev,
                               const char *propname) {
  return fwnode_property_read_bool(dev_fwnode(dev), propname);
}

struct fwnode_handle *
fwnode_get_named_child_node(const struct fwnode_handle *fwnode,
                            const char *childname) {
  return fwnode_call_ptr_op(fwnode, get_named_child_node, childname);
}

struct fwnode_handle *fwnode_handle_get(struct fwnode_handle *fwnode) {
  if (!fwnode_has_op(fwnode, get))
    return fwnode;

  return fwnode_call_ptr_op(fwnode, get);
}
