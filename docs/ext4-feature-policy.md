# ext4 Feature Mount Policy

The ext4 backend shares block allocation and journaling code with ext2/ext3.
It must validate feature flags before exposing the filesystem or allowing
metadata writes.

## Current policy

- Unknown `INCOMPAT` bits reject the mount. The driver accepts `FILETYPE`,
  `RECOVER`, `EXTENTS`, and `64BIT`; ext4 mounts require `EXTENTS`.
- `64BIT` reads the three high superblock block counters and extended group
  descriptors using the advertised descriptor stride. Physical block numbers
  stay 64-bit through the cache, block I/O, and JBD tags. Extent records store
  48-bit physical addresses, so extent allocation is limited to that format's
  address range; legacy inode block maps remain limited to 32-bit addresses.
  Group geometry is checked against bitmap capacity and the descriptor's
  counter width. Superblock writeback preserves the rest of the containing
  filesystem block, including block 0 on filesystems larger than 1 KiB.
- The journal accepts the JBD2 `64BIT` and `REVOKE` incompat features, writes
  extended block tags, handles deleted tags, and applies revokes by transaction
  sequence during recovery. Other JBD2 incompat features still reject journal
  setup.
- `RECOVER` without `HAS_JOURNAL` rejects the mount. Journal setup and replay
  finish before the root inode is exposed.
- `SPARSE_SUPER` and the legacy `BTREE_DIR` read-only-compatible bits are
  supported. Any other `RO_COMPAT` bit forces a read-only mount.
- `LARGE_FILE` remains unsupported because the VFS and filesystem APIs use
  32-bit file sizes and offsets. Project image builders disable this bit for
  their root images; externally formatted filesystems carrying it mount
  read-only.
- A read-only mount rejects writes, metadata changes, allocation, and journal
  transactions through the VFS and block-write paths. The VFS mount table and
  `statfs` report the read-only state.
- If a read-only mount finds a dirty journal, it fails instead of replaying it;
  replay would write to the device. Journal setup or recovery errors also reject
  the mount.
- Unknown `COMPAT` bits are ignored, as their on-disk contract permits an
  implementation to mount without understanding them.

The ext2 mount path uses the same feature check because the mount syscall can
fall back from ext4 to ext2. That fallback must not bypass an ext4 incompatibility
decision.

To add writable support for another feature, implement its on-disk behavior
first, add it to the supported mask, and cover the read/write and recovery
paths before changing this policy.
