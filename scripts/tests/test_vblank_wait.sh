#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "$0")/../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
source_file=${1:-"$repo_dir/kernel/src/fb/vblank.c"}
sed '/^#include "/d' "$source_file" > "$test_dir/vblank.inc"
cc -std=gnu11 -O2 -Wall -Wextra -Werror \
  -DVBLANK_SOURCE="\"$test_dir/vblank.inc\"" \
  "$repo_dir/scripts/tests/test_vblank_wait.c" -o "$test_dir/test"
"$test_dir/test"
