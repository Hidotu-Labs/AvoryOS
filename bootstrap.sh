#!/bin/sh

set -e

# Repository URLs
CC_RUNTIME_URL="https://github.com/osdev0/cc-runtime.git"
LIMINE_PROTOCOL_URL="https://github.com/limine-bootloader/limine-protocol.git"
FREESTND_C_HDRS_URL="https://github.com/osdev0/freestnd-c-hdrs-0bsd.git"
AETHERDE_URL="https://github.com/Hidotu-Labs/AetherDE.git"

# Function to setup a dependency
setup_dep() {
    local dir=$1
    local url=$2

    echo "--- Setting up $dir ---"

    if [ ! -d "$dir" ] || [ -z "$(ls -A "$dir" 2>/dev/null)" ]; then
        echo "Cloning $url into $dir..."
        rm -rf "$dir"
        git clone "$url" "$dir" --depth=1
    else
        echo "$dir already exists and is not empty. Skipping clone."
    fi
}

mkdir -p kernel

setup_dep "kernel/cc-runtime" "$CC_RUNTIME_URL"
setup_dep "kernel/limine-protocol" "$LIMINE_PROTOCOL_URL"
setup_dep "kernel/freestnd-c-hdrs" "$FREESTND_C_HDRS_URL"
setup_dep "AetherDE" "$AETHERDE_URL"
echo "--- Setting up prebuilt GCC cross-compiler ---"
chmod +x scripts/glibc-toolchain.sh
./scripts/glibc-toolchain.sh

if [ "${AVORY_BOOTSTRAP_SKIP_ROOTFS:-0}" != "1" ]; then
    echo "--- Setting up Axon root filesystem ---"
    bash scripts/setup-rootfs.sh
    mkdir -p build/arch
    touch build/arch/.built
fi

echo "All dependencies have been set up."
