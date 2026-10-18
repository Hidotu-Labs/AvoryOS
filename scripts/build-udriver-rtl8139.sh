#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
GLIBC_SYSROOT="$ROOT_DIR/toolchain/glibc-sysroot"
CROSS_CC="$ROOT_DIR/toolchain/x86_64-linux-glibc/bin/x86_64-linux-gcc"
OUTPUT="$ROOT_DIR/userland/8139too-udrv.elf"
TEMP_OUTPUT="$ROOT_DIR/userland/.8139too-udrv.elf.tmp"
COMMON_SOURCES="$ROOT_DIR/userland/linux_compat/src/ascent_compat.c $ROOT_DIR/userland/drivers/net/ethernet/realtek/8139too.c"
COMMON_FLAGS="-O2 -Wall -Wextra -fno-stack-protector -I$ROOT_DIR/include -I$ROOT_DIR/userland/linux_compat/include"

rm -f "$TEMP_OUTPUT"
"$CROSS_CC" $COMMON_FLAGS --sysroot="$GLIBC_SYSROOT" \
  -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
  $COMMON_SOURCES -o "$TEMP_OUTPUT"
mv "$TEMP_OUTPUT" "$OUTPUT"

