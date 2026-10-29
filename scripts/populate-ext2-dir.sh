#!/bin/sh
# Keep the existing entry point for make recipes and external callers.
set -eu
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
exec python3 "$SCRIPT_DIR/populate-ext2-dir.py" "$@"
