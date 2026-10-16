#!/bin/bash
# Build the guest's Plasma 6.7.5 library with the external wired fallback fix.
# Uses host development libraries. Does not edit disk.img or the running VM.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
work="$repo/build/plasma-network-icon"
mkdir -p "$work"
fetch() {
    local name=$1 url=$2 tag=$3
    if [ ! -d "$work/$name/.git" ]; then
        git clone --depth 1 --branch "$tag" "$url" "$work/$name"
    fi
    [ "$(git -C "$work/$name" describe --tags --exact-match HEAD)" = "$tag" ]
}
fetch source https://invent.kde.org/plasma/plasma-nm.git v6.7.5
fetch ecm https://invent.kde.org/frameworks/extra-cmake-modules.git v6.29.0
patch="$repo/scripts/patches/plasma-nm-external-wired.patch"
if ! git -C "$work/source" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$work/source" apply --check "$patch"
    git -C "$work/source" apply "$patch"
fi
cmake -S "$work/ecm" -B "$work/ecm-build" -DCMAKE_INSTALL_PREFIX="$work/ecm-prefix" -DBUILD_DOC=OFF -DBUILD_TESTING=OFF
cmake --install "$work/ecm-build"
cmake -S "$work/source" -B "$work/obj" -DCMAKE_PREFIX_PATH="$work/ecm-prefix" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG -flto=auto" -DCMAKE_SHARED_LINKER_FLAGS_RELEASE="-flto=auto" -DBUILD_OPENCONNECT=OFF -DBUILD_TESTING=OFF
cmake --build "$work/obj" --target plasmanm_internal -j "${AVORY_BUILD_JOBS:-4}"
cmake -S "$repo/scripts/tests/network-icon-probe" -B "$work/probe" -DPLASMA_NM_SOURCE="$work/source" -DPLASMA_NM_BUILD="$work/obj"
cmake --build "$work/probe" -j "${AVORY_BUILD_JOBS:-4}"
mkdir -p "$work/overlay/usr/lib"
objcopy --strip-unneeded "$work/obj/bin/libplasmanm_internal.so" "$work/overlay/usr/lib/libplasmanm_internal.so"
printf 'Prepared Plasma library: %s\n' "$work/overlay/usr/lib/libplasmanm_internal.so"
