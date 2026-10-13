#!/bin/sh
# Populate directory tree inside an ext2 image using debugfs batch mode.
set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=$(dirname "$SCRIPT_DIR")

IMG_PATH=${1:-$ROOT_DIR/disk.img}
# Ensure IMG_PATH is absolute
if [ -f "$IMG_PATH" ]; then
    IMG_PATH=$(readlink -f "$IMG_PATH")
else
    # If it doesn't exist, readlink -f might fail or return nothing.
    # Just make it absolute manually if it's relative.
    case "$IMG_PATH" in
        /*) ;;
        *) IMG_PATH="$PWD/$IMG_PATH" ;;
    esac
fi

SRC_DIR=${2:-}
[ -n "$SRC_DIR" ] && SRC_DIR=$(readlink -f "$SRC_DIR")
DST_DIR=${3:-}

if [ -z "$SRC_DIR" ] || [ -z "$DST_DIR" ]; then
  echo "Usage: $0 <img_path> <src_dir> <dst_dir>"
  exit 1
fi

# Remove trailing slashes
SRC_DIR=${SRC_DIR%/}
DST_DIR=${DST_DIR%/}

if [ ! -d "$SRC_DIR" ]; then
  echo "Missing source directory: $SRC_DIR" >&2
  exit 1
fi

# Host ownership is flattened by the package installer's user namespace.
# Recover the rules directory's declared root:polkitd ownership from target
# accounts, rather than host IDs or a hard-coded daemon GID. Validate before
# any image mutation. Do not give the daemon permission to replace rules.
POLKIT_RULES_GID=""
if [ -z "$DST_DIR" ] && [ -d "$SRC_DIR/etc/polkit-1/rules.d" ]; then
    if [ ! -f "$SRC_DIR/etc/group" ]; then
        echo "Missing target /etc/group for polkit rules ownership" >&2
        exit 1
    fi
    POLKIT_RULES_GID=$(awk -F: '$1 == "polkitd" {print $3; exit}' "$SRC_DIR/etc/group")
    case "$POLKIT_RULES_GID" in
        ''|*[!0-9]*) echo "Missing or invalid target polkitd GID" >&2; exit 1 ;;
    esac
fi

CMDS_FILE=$(mktemp)

# Resolve debugfs binary early so the stale-file wipe below can use it.
DEBUGFS_BIN=debugfs
if [ -x /run/host/usr/bin/debugfs ]; then
  DEBUGFS_BIN=/run/host/usr/bin/debugfs
fi

# Start generating commands
echo "cd /" > "$CMDS_FILE"

# Wipe the destination tree so stale files from previous builds (e.g. old
# glibc headers left behind by a tool-chain package that was later removed)
# are not present in the image alongside the fresh musl rootfs.
# Use a separate one-shot invocation so a missing rm_rf command (e2fsprogs
# < 1.44) does not abort the population; the worst case is stale files survive.
if [ "$DST_DIR" != "/" ]; then
    "$DEBUGFS_BIN" -w "$IMG_PATH" -R "rm_rf ${DST_DIR}" >/dev/null 2>&1 || true
fi

ensure_dir_cmds() {
  cur=""
  old_ifs=$IFS
  IFS='/'
  # shellcheck disable=SC2086
  set -- $1
  IFS=$old_ifs

  for comp in "$@"; do
    [ -n "$comp" ] || continue
    if [ -z "$cur" ]; then
      cur="$comp"
    else
      cur="$cur/$comp"
    fi
    echo "mkdir $cur" >> "$CMDS_FILE"
  done
}

ensure_dir_cmds "$DST_DIR"

# Space-separated SRC_DIR-relative prefixes excluded from the image to keep
# it fitting comfortably: build headers, documentation and static archives
# are never read at runtime (rebuilds compile against the build tree, not
# the image). Override from the environment to adjust, e.g.
# EXCLUDE_IMAGE_PATHS="usr/include" for a fuller image.
EXCLUDE_IMAGE_PATHS="${EXCLUDE_IMAGE_PATHS:-usr/include usr/share/doc usr/share/man}"

# Assemble find(1) prune primaries from the exclusion list.
PRUNE_EXPR=""
for _ex in $EXCLUDE_IMAGE_PATHS; do
  # shellcheck disable=SC2086
  if [ -z "$PRUNE_EXPR" ]; then
    PRUNE_EXPR="-path ./$_ex"
  else
    PRUNE_EXPR="$PRUNE_EXPR -o -path ./$_ex"
  fi
done

(
  cd "$SRC_DIR"
  # First create all directories
  # shellcheck disable=SC2086
  find . \( $PRUNE_EXPR \) -prune -o -type d -print | while IFS= read -r dir_path; do
    rel_path=${dir_path#./}
    [ "$rel_path" = "." ] && continue
    echo "mkdir $DST_DIR/$rel_path" >> "$CMDS_FILE"
    mode=$(stat -c %a "$dir_path")
    # S_IFDIR (0040000) + permission bits, including setuid/setgid/sticky.
    # Old code used 040$mode which breaks 4-digit modes (e.g. 2755 ->
    # 0402755 instead of 042755).
    full_mode=$(printf "0%o" $((8#40000 + 8#$mode)))
    echo "set_inode_field $DST_DIR/$rel_path mode $full_mode" >> "$CMDS_FILE"
    if [ "$rel_path" = "etc/polkit-1/rules.d" ] && [ -n "$POLKIT_RULES_GID" ]; then
      echo "set_inode_field /$rel_path uid 0" >> "$CMDS_FILE"
      echo "set_inode_field /$rel_path gid $POLKIT_RULES_GID" >> "$CMDS_FILE"
      echo "set_inode_field /$rel_path mode 040750" >> "$CMDS_FILE"
    fi
  done

  # Then write files and symlinks (static archives are build-only, like
  # the excluded include tree above).
  # shellcheck disable=SC2086
  find . \( $PRUNE_EXPR \) -prune -o \( -type f ! -name '*.a' -o -type l \) -print | while IFS= read -r file_path; do
    rel_path=${file_path#./}
    if [ -L "$file_path" ]; then
      target=$(readlink "$file_path")
      echo "symlink $DST_DIR/$rel_path $target" >> "$CMDS_FILE"
    else
      echo "rm $DST_DIR/$rel_path" >> "$CMDS_FILE"
      echo "write $SRC_DIR/$rel_path $DST_DIR/$rel_path" >> "$CMDS_FILE"
      mode=$(stat -c %a "$file_path")
      # S_IFREG (0100000) + permission bits, including setuid/setgid/sticky.
      # Old code used 0100$mode which breaks 4-digit modes (e.g. 4755 ->
      # 01004755 instead of 0104755).
      full_mode=$(printf "0%o" $((8#100000 + 8#$mode)))
      echo "set_inode_field $DST_DIR/$rel_path mode $full_mode" >> "$CMDS_FILE"
      mtime=$(stat -c %Y "$file_path")
      echo "set_inode_field $DST_DIR/$rel_path mtime @$mtime" >> "$CMDS_FILE"
    fi
  done

  # Creating children changes directory mtimes. Restore them only after the
  # complete tree has been populated so metadata-based caches (notably
  # Fontconfig) remain valid in the image. (-prune is ineffective with
  # -depth, so excluded prefixes are skipped in shell here.)
  find . -depth -type d | while IFS= read -r dir_path; do
    rel_path=${dir_path#./}
    [ "$rel_path" = "." ] && continue
    _skip=""
    for _ex in $EXCLUDE_IMAGE_PATHS; do
      case "$rel_path" in
        "$_ex"|"$_ex"/*) _skip=1; break;;
      esac
    done
    [ -n "$_skip" ] && continue
    mtime=$(stat -c %Y "$dir_path")
    echo "set_inode_field $DST_DIR/$rel_path mtime @$mtime" >> "$CMDS_FILE"
  done
)

# Flatpak/container development shells may expose debugfs only through the
# host filesystem. Prefer that binary when available so a successful setup
# cannot silently leave the disk image unchanged.
DEBUGFS_BIN=debugfs
if [ -x /run/host/usr/bin/debugfs ]; then
  DEBUGFS_BIN=/run/host/usr/bin/debugfs
fi

# debugfs can exit successfully after individual writes fail. Keep its batch
# diagnostics and reject space/allocation/write failures explicitly.
BATCH_LOG=$(mktemp)
if ! "$DEBUGFS_BIN" -w -f "$CMDS_FILE" "$IMG_PATH" > "$BATCH_LOG" 2>&1; then
  echo "Failed to populate ext2 image with $DEBUGFS_BIN" >&2
  tail -20 "$BATCH_LOG" >&2
  rm -f "$CMDS_FILE" "$BATCH_LOG"
  exit 1
fi
if grep -Ei 'could not allocate|no space left|could not write|short write' "$BATCH_LOG" >&2; then
  echo "Image population failed: enlarge DISK_SIZE_MIB and rebuild disk.img" >&2
  rm -f "$CMDS_FILE" "$BATCH_LOG"
  exit 1
fi

rm -f "$CMDS_FILE" "$BATCH_LOG"
