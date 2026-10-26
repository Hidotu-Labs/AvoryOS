#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "$0")/../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
source_file=${1:-"$repo_dir/kernel/src/fs/ext4/ext4_extent.c"}
# Keep the production implementation; replace only platform includes with
# the in-memory disk and transaction interfaces declared by the C harness.
sed '/^#include "/d' "$source_file" > "$test_dir/extent.inc"
cc -O2 -Wall -Wextra -Werror -Wno-unused-function \
  -fsanitize=address,undefined -DEXTENT_SOURCE="\"$test_dir/extent.inc\"" \
  "$repo_dir/scripts/tests/test_ext4_extent_journal.c" -o "$test_dir/test"
ASAN_OPTIONS=detect_leaks=0 "$test_dir/test"
