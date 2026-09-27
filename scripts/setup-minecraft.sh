#!/usr/bin/env bash
# scripts/setup-minecraft.sh -- Stage Minecraft 1.16.5 into AvoryOS
#
# Prerequisites:
#   1. Run ./scripts/setup-alpine.sh first  (provides Alpine rootfs, disk.img, OpenJDK 17)
#   2. Network access on the first run: the client JAR is downloaded from the
#      mcversions.net link, libraries/assets from Mojang (includes all 2,336
#      OGG sound files of asset index 1.16)
#
# Usage:
#   ./scripts/setup-minecraft.sh [--expand-disk] [--username NAME] [--jar PATH]
#
# Options:
#   --expand-disk   Grow disk.img to 8 GiB
#   --username NAME Offline username written into the launcher (default: Player)
#   --jar PATH      Use a locally saved 1.16.5 client JAR (e.g. downloaded from
#                   https://mcversions.net/download/1.16.5) instead of fetching it

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="${ROOT_DIR}/disk.img"
BUILD_DIR="${ROOT_DIR}/build/alpine"
ROOTFS_DIR="${BUILD_DIR}/rootfs"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"

MC_VERSION="1.16.5"
# Client JAR published on mcversions.net (https://mcversions.net/download/1.16.5);
# piston-data.mojang.com addresses objects by their SHA-1.
MC_CLIENT_URL="https://piston-data.mojang.com/v1/objects/37fd3c903861eeff3bc24b71eed48f828b5269c8/client.jar"
MC_CLIENT_SHA1="37fd3c903861eeff3bc24b71eed48f828b5269c8"
MC_ASSET_INDEX="1.16"
VERSION_MANIFEST_URL="https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"

MC_JAR_OVERRIDE=""
EXPAND_DISK=0
MC_USERNAME="Player"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --expand-disk) EXPAND_DISK=1; shift ;;
        --username)    MC_USERNAME="${2:-Player}"; shift 2 ;;
        --jar)         MC_JAR_OVERRIDE="${2:-}"; shift 2 ;;
        *) echo "[!] Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# ─────────────────────────────────────────────────────────────────────────────
# Sanity checks
# ─────────────────────────────────────────────────────────────────────────────
[ ! -d "${ROOTFS_DIR}/etc" ] && {
    echo "[!] Alpine rootfs not found. Run ./scripts/setup-alpine.sh first."
    exit 1
}
[ ! -f "${DISK_IMG}" ] && {
    echo "[!] disk.img not found. Run 'make disk.img' first."
    exit 1
}

# ─────────────────────────────────────────────────────────────────────────────
# Locate or install OpenJDK 17 in rootfs
# ─────────────────────────────────────────────────────────────────────────────
ALPINE_BRANCH="v3.21"
APK_CACHE="${BUILD_DIR}"

install_apk_mc() {
    local PKG_NAME="$1"
    local REPO="$2"
    local BRANCH="${3:-${ALPINE_BRANCH}}"
    local PKG_MARKER="${ROOTFS_DIR}/etc/avoryos-pkg/${BRANCH}-${REPO}-${PKG_NAME}"

    if [ -f "${PKG_MARKER}" ]; then
        return 0
    fi

    echo "[*] Installing ${PKG_NAME} from ${REPO} (${BRANCH})..."

    local ESCAPED
    ESCAPED=$(printf '%s' "${PKG_NAME}" | sed 's/\./\\./g;s/+/\\+/g')
    local APK_FILENAME
    APK_FILENAME=$(curl -sL "https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/${REPO}/x86_64/" \
        | grep -oP ">${ESCAPED}-[0-9][^<]*\.apk<" | sed 's/>//;s/<//' | sort -V | tail -n 1)

    [ -z "${APK_FILENAME}" ] && {
        echo "[!] Could not find package ${PKG_NAME} in ${REPO} (${BRANCH})" >&2
        return 1
    }

    local APK_URL="https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/${REPO}/x86_64/${APK_FILENAME}"
    if [ ! -f "${APK_CACHE}/${APK_FILENAME}" ]; then
        echo "    Downloading ${APK_URL}..."
        curl -L "${APK_URL}" -o "${APK_CACHE}/${APK_FILENAME}"
    fi

    tar --ignore-zeros -xzf "${APK_CACHE}/${APK_FILENAME}" -C "${ROOTFS_DIR}" \
        --warning=no-unknown-keyword 2>/dev/null || true

    mkdir -p "${ROOTFS_DIR}/etc/avoryos-pkg"
    touch "${PKG_MARKER}"
}

find_java() {
    JAVA_HOME_ROOTFS=""
    for cand in \
        "${ROOTFS_DIR}/usr/lib/jvm/java-17-openjdk" \
        "${ROOTFS_DIR}/usr/lib/jvm/java-17" \
        "${ROOTFS_DIR}/usr/lib/jvm/default-jvm"; do
        if [ -d "${cand}" ]; then
            JAVA_HOME_ROOTFS="${cand}"
            return 0
        fi
    done
    if [ -z "${JAVA_HOME_ROOTFS}" ]; then
        JAVA_HOME_ROOTFS=$(ls -d "${ROOTFS_DIR}/usr/lib/jvm"/java-17* 2>/dev/null | head -1 || true)
    fi
    return 0
}

find_java
if [ -z "${JAVA_HOME_ROOTFS}" ]; then
    echo "[*] OpenJDK 17 not found in rootfs. Installing now..."
    install_apk_mc "mesa-gl" "main"
    install_apk_mc "mesa-glapi" "main"
    install_apk_mc "mesa-gles" "main"
    install_apk_mc "mesa-egl" "main"
    install_apk_mc "freeglut" "community"
    install_apk_mc "jemalloc" "main"
    install_apk_mc "java-common"            "community"
    install_apk_mc "openjdk17-jre-headless" "community"
    install_apk_mc "openjdk17-jre"          "community"
    install_apk_mc "java-cacerts"           "community"
    find_java
fi

[ -z "${JAVA_HOME_ROOTFS}" ] && {
    echo "[!] Failed to find or install OpenJDK 17. Aborting." >&2
    exit 1
}

JAVA_HOME_GUEST="${JAVA_HOME_ROOTFS#"${ROOTFS_DIR}"}"
echo "[+] Using JDK: ${JAVA_HOME_GUEST}"

# Minecraft's bundled OpenAL is an old glibc build. Ensure a musl-native
# OpenAL Soft is present even when Java was already installed by setup-alpine.
# Dependencies are not resolved automatically, so install the library package
# explicitly before the optional OpenAL utilities package.
install_apk_mc "openal-soft-libs" "community"
install_apk_mc "openal-soft" "community"

# Minecraft 1.16.5 uses LWJGL 3/GLFW, which dlopen()s these X11 client
# libraries at window-creation time. Install them explicitly: dependencies
# are not resolved automatically.
install_apk_mc "libx11"       "main"
install_apk_mc "libxext"      "main"
install_apk_mc "libxi"        "main"
install_apk_mc "libxrandr"    "main"
install_apk_mc "libxcursor"   "main"
install_apk_mc "libxinerama"  "main"
install_apk_mc "libxxf86vm"   "main"

# All JVM internal .so files use RPATH=$ORIGIN/../lib which AvoryOS's musl
# doesn't reliably resolve. Copy every .so from the JVM lib dir into /usr/lib/
# so musl's hardcoded search path always finds them.
echo "[*] Copying JVM internal libraries to /usr/lib/..."
find "${JAVA_HOME_ROOTFS}/lib" -name '*.so' -exec cp -f {} "${ROOTFS_DIR}/usr/lib/" \;
JVM_LIBS_COUNT=$(find "${JAVA_HOME_ROOTFS}/lib" -name '*.so' | wc -l)
echo "[+] Copied ${JVM_LIBS_COUNT} JVM .so files → /usr/lib/"

# libjli.so looks for jvm.cfg relative to its own location.
# Since it's now in /usr/lib/, copy jvm.cfg there too.
cp -f "${JAVA_HOME_ROOTFS}/lib/jvm.cfg" "${ROOTFS_DIR}/usr/lib/jvm.cfg"
echo "[+] Copied jvm.cfg → /usr/lib/"

# jvm.cfg references '-server KNOWN', so libjvm.so must also be in /usr/lib/.
cp -f "${JAVA_HOME_ROOTFS}/lib/server/libjvm.so" "${ROOTFS_DIR}/usr/lib/libjvm.so"
echo "[+] Copied server/libjvm.so → /usr/lib/"

# libjli.so resolves the server JVM at $libdir/server/libjvm.so relative to
# its own location (/usr/lib/), so create /usr/lib/server/ as well.
mkdir -p "${ROOTFS_DIR}/usr/lib/server"
cp -f "${JAVA_HOME_ROOTFS}/lib/server/libjvm.so" "${ROOTFS_DIR}/usr/lib/server/libjvm.so"
echo "[+] Copied server/libjvm.so → /usr/lib/server/"

# ─────────────────────────────────────────────────────────────────────────────
# 1. Download Minecraft 1.16.5 libraries + Linux natives
#    (version metadata from Mojang; client JAR link comes from mcversions.net)
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [1/4] Fetching Minecraft ${MC_VERSION} libraries ==="

LIBS_DIR="${BUILD_DIR}/mc1165-libs"
NATIVES_BUILD_DIR="${BUILD_DIR}/mc1165-natives"
NATIVES_LIST="${BUILD_DIR}/mc1165-natives.list"
MC_LIBS_ROOTFS="${ROOTFS_DIR}/opt/minecraft/libs"
MC_NATIVES_ROOTFS="${ROOTFS_DIR}/opt/minecraft/natives"
mkdir -p "${LIBS_DIR}" "${NATIVES_BUILD_DIR}" "${MC_LIBS_ROOTFS}" "${MC_NATIVES_ROOTFS}"

if ! command -v python3 >/dev/null 2>&1; then
    echo "[!] python3 is required to download Minecraft libraries and assets." >&2
    exit 1
fi

# Remove anything staged by an older setup run: leftover LWJGL 2 and
# log4j 2.0-beta9 JARs from 1.8.9 must not end up on the 1.16.5 classpath.
rm -f "${MC_LIBS_ROOTFS:?}"/*.jar
rm -f "${MC_NATIVES_ROOTFS:?}"/*.so

# Helper: download a file if not already cached
dl() {
    local url="$1" dest="$2"
    if [ ! -f "${dest}" ]; then
        echo "[*] Downloading $(basename "${dest}")..."
        curl -sSL --retry 3 "${url}" -o "${dest}" || { echo "[!] FAILED: ${url}" >&2; return 1; }
    fi
}

# Resolve every classpath JAR and Linux natives JAR from Mojang's own 1.16.5
# version metadata (same data mcversions.net links against), applying Mojang's
# OS rules so macOS/Windows-only libraries never reach the classpath.
echo "[*] Fetching Minecraft ${MC_VERSION} version metadata..."
python3 - "${MC_VERSION}" "${VERSION_MANIFEST_URL}" "${LIBS_DIR}" \
    "${NATIVES_BUILD_DIR}" "${NATIVES_LIST}" <<'LIB_DOWNLOADER_EOF'
import concurrent.futures
import hashlib
import json
import sys
import time
import urllib.request
from pathlib import Path

VERSION, MANIFEST_URL, LIBS_DIR, NATIVES_DIR, NATIVES_LIST = sys.argv[1:6]
LIBS_DIR = Path(LIBS_DIR)
NATIVES_DIR = Path(NATIVES_DIR)
USER_AGENT = {"User-Agent": "AvoryOS-Minecraft-Setup/1"}


def fetch(url, retries=3):
    last_error = None
    for attempt in range(retries):
        try:
            request = urllib.request.Request(url, headers=USER_AGENT)
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.read()
        except Exception as error:
            last_error = error
            time.sleep(attempt + 1)
    raise RuntimeError(f"failed to download {url}: {last_error}")


def sha1_hex(data):
    return hashlib.sha1(data).hexdigest()


def rules_allow_linux(library):
    """Evaluate Mojang's library rules for (linux, x86_64)."""
    rules = library.get("rules")
    if not rules:
        return True
    allowed = False
    for rule in rules:
        os_spec = rule.get("os", {})
        matches = os_spec.get("name") in (None, "linux")
        if os_spec.get("arch") not in (None, "amd64", "x86_64"):
            matches = False  # e.g. {"arch": "x86"} only applies to 32-bit
        if matches:
            allowed = rule.get("action", "allow") == "allow"
    return allowed


manifest = json.loads(fetch(MANIFEST_URL))
entry = next((v for v in manifest["versions"] if v["id"] == VERSION), None)
if entry is None:
    raise SystemExit(f"Minecraft {VERSION} is missing from Mojang's version manifest")

metadata_raw = fetch(entry["url"])
if sha1_hex(metadata_raw) != entry["sha1"]:
    raise SystemExit(f"SHA-1 mismatch for the {VERSION} version metadata")
metadata = json.loads(metadata_raw)

# Keyed by destination so repeated coordinates (LWJGL appears several times)
# are only downloaded once.
jobs = {}
native_jars = set()
for library in metadata["libraries"]:
    if not rules_allow_linux(library):
        continue
    downloads = library.get("downloads", {})
    artifact = downloads.get("artifact")
    if artifact is None:
        # Coordinates without a "downloads" block still live on the Mojang CDN.
        group, name, version = library["name"].split(":")[:3]
        rel_path = f"{group.replace('.', '/')}/{name}/{version}/{name}-{version}.jar"
        artifact = {
            "path": rel_path,
            "url": f"https://libraries.minecraft.net/{rel_path}",
            "sha1": None,
        }
    jobs[LIBS_DIR / Path(artifact["path"]).name] = (artifact["url"], artifact.get("sha1"))

    natives_key = library.get("natives", {}).get("linux")
    if natives_key:
        classifier = downloads.get("classifiers", {}).get(natives_key)
        if classifier is None:
            raise SystemExit(f"{library['name']} declares Linux natives but ships no classifier")
        dest = NATIVES_DIR / Path(classifier["path"]).name
        jobs[dest] = (classifier["url"], classifier.get("sha1"))
        native_jars.add(dest)


def is_valid(dest, expected_sha1):
    if not dest.is_file():
        return False
    if expected_sha1 is None:
        return True
    return sha1_hex(dest.read_bytes()) == expected_sha1


def download(item):
    dest, (url, expected_sha1) = item
    if is_valid(dest, expected_sha1):
        return False
    data = fetch(url)
    if expected_sha1 is not None and sha1_hex(data) != expected_sha1:
        raise SystemExit(f"SHA-1 mismatch downloading {url}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    temporary = dest.with_name(dest.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(dest)
    return True


downloaded = 0
failures = []
with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
    futures = [executor.submit(download, item) for item in jobs.items()]
    for future in concurrent.futures.as_completed(futures):
        try:
            was_downloaded = future.result()
        except Exception as error:
            failures.append(str(error))
        else:
            downloaded += was_downloaded

if failures:
    for failure in failures:
        print(f"[!] {failure}", file=sys.stderr)
    raise SystemExit("Could not download all Minecraft 1.16.5 libraries")

native_jars_sorted = sorted(native_jars)
Path(NATIVES_LIST).write_text("".join(f"{path}\n" for path in native_jars_sorted))

artifact_count = sum(1 for dest in jobs if dest.parent == LIBS_DIR)
print(
    f"[+] {artifact_count} classpath JARs, {len(native_jars)} native JARs "
    f"({downloaded} downloaded)"
)
LIB_DOWNLOADER_EOF

# ── Extract the Linux natives (LWJGL 3.2.2, GLFW, OpenAL, STB, ...) ─────────
echo "[*] Extracting Linux native .so files..."
while IFS= read -r nat_jar; do
    [ -n "${nat_jar}" ] || continue
    if [ ! -f "${nat_jar}" ]; then
        echo "[!] Missing native JAR: ${nat_jar}" >&2
        exit 1
    fi
    unzip -oj "${nat_jar}" "*.so" -d "${MC_NATIVES_ROOTFS}" 2>/dev/null || true
done < "${NATIVES_LIST}"

# LWJGL bundles its own glibc-built OpenAL. Replace it with Alpine's
# musl-native OpenAL Soft (LWJGL 3 probes libopenal.so.1/libopenal.so and
# must find the musl build under every one of those names).
SYSTEM_OPENAL="${ROOTFS_DIR}/usr/lib/libopenal.so.1"
if [ ! -f "${SYSTEM_OPENAL}" ]; then
    echo "[!] Alpine OpenAL Soft library was not installed at /usr/lib/libopenal.so.1" >&2
    exit 1
fi
if readelf --version-info "${SYSTEM_OPENAL}" 2>/dev/null | grep -q 'GLIBC_'; then
    echo "[!] Refusing to stage a glibc OpenAL library in the musl rootfs." >&2
    exit 1
fi
for openal_name in libopenal.so libopenal.so.1 libopenal64.so; do
    cp -Lf "${SYSTEM_OPENAL}" "${MC_NATIVES_ROOTFS}/${openal_name}"
done
echo "[+] Replaced bundled OpenAL with Alpine's musl-native OpenAL Soft"

# Copy all library JARs into rootfs
cp -f "${LIBS_DIR}"/*.jar "${MC_LIBS_ROOTFS}/" 2>/dev/null || true

NATIVES_COUNT=$(find "${MC_NATIVES_ROOTFS}" -name '*.so' | wc -l)
LIBS_COUNT=$(find "${MC_LIBS_ROOTFS}" -name '*.jar' | wc -l)
echo "[+] Staged ${LIBS_COUNT} library JARs → /opt/minecraft/libs/"
echo "[+] Staged ${NATIVES_COUNT} native .so files → /opt/minecraft/natives/"

# The official launcher normally downloads the asset index and hashed object
# store. This custom launcher bypasses it, so fetch and verify those assets here.
# Index 1.16 holds 2,615 objects (334 MB) including all 2,336 OGG sounds.
ASSETS_BUILD_DIR="${BUILD_DIR}/mc1165-assets"
MC_ASSETS_ROOTFS="${ROOTFS_DIR}/opt/minecraft/assets"

echo "[*] Fetching Minecraft ${MC_VERSION} asset index ${MC_ASSET_INDEX} and objects (incl. sounds)..."
python3 - "${ASSETS_BUILD_DIR}" "${MC_VERSION}" "${MC_ASSET_INDEX}" <<'ASSET_DOWNLOADER_EOF'
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import urllib.request

VERSION = sys.argv[2]
INDEX_ID = sys.argv[3]
MANIFEST_URL = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
ASSET_OBJECT_URL = "https://resources.download.minecraft.net/{prefix}/{digest}"
CACHE = Path(sys.argv[1])
INDEXES = CACHE / "indexes"
OBJECTS = CACHE / "objects"
INDEXES.mkdir(parents=True, exist_ok=True)
OBJECTS.mkdir(parents=True, exist_ok=True)


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "AvoryOS-Minecraft-Setup/1"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read()


def checked_fetch(url, expected_hash):
    for attempt in range(3):
        try:
            data = fetch(url)
            actual_hash = hashlib.sha1(data).hexdigest()
            if actual_hash != expected_hash:
                raise RuntimeError(f"SHA-1 mismatch: expected {expected_hash}, got {actual_hash}")
            return data
        except Exception:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


manifest = json.loads(fetch(MANIFEST_URL))
try:
    version_entry = next(entry for entry in manifest["versions"] if entry["id"] == VERSION)
except StopIteration:
    raise SystemExit(f"Minecraft {VERSION} is missing from Mojang's version manifest")

version_data = json.loads(checked_fetch(version_entry["url"], version_entry["sha1"]))
asset_index = version_data["assetIndex"]
if asset_index["id"] != INDEX_ID:
    raise SystemExit(f"Expected asset index {INDEX_ID}, got {asset_index['id']}")

index_data = checked_fetch(asset_index["url"], asset_index["sha1"])
index_path = INDEXES / f"{INDEX_ID}.json"
index_path.write_bytes(index_data)
index = json.loads(index_data)


def download_object(item):
    logical_name, metadata = item
    digest = metadata["hash"]
    destination = OBJECTS / digest[:2] / digest
    if destination.is_file():
        with destination.open("rb") as existing:
            if hashlib.sha1(existing.read()).hexdigest() == digest:
                return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    data = checked_fetch(ASSET_OBJECT_URL.format(prefix=digest[:2], digest=digest), digest)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_bytes(data)
    os.replace(temporary, destination)
    return True


items = list(index["objects"].items())
downloaded = 0
with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
    futures = [executor.submit(download_object, item) for item in items]
    for completed, future in enumerate(concurrent.futures.as_completed(futures), 1):
        if future.result():
            downloaded += 1
        if completed % 250 == 0 or completed == len(futures):
            print(f"    Verified {completed}/{len(futures)} asset objects", flush=True)

sound_count = sum(name.endswith(".ogg") for name in index["objects"])
print(
    f"[+] Asset index {INDEX_ID} ready: {len(items)} objects, "
    f"{sound_count} OGG sounds ({downloaded} downloaded)"
)
ASSET_DOWNLOADER_EOF

rm -rf "${MC_ASSETS_ROOTFS}"
mkdir -p "${MC_ASSETS_ROOTFS}"
cp -a "${ASSETS_BUILD_DIR}/." "${MC_ASSETS_ROOTFS}/"
echo "[+] Staged Minecraft assets (all OGG sounds included) → /opt/minecraft/assets/"

# Mojang's LWJGL natives reference glibc fortify entry points that musl does
# not export. Without them, lazy PLT calls jump into an unrebased trampoline
# (e.g. 0x86ae); with RTLD_NOW, loading fails with "symbol not found". Provide
# bounds-checked musl wrappers and force immediate relocation for dlopen'd DSOs.
NATIVE_COMPAT_SRC="${BUILD_DIR}/native-compat.c"
NATIVE_COMPAT_LIB="${ROOTFS_DIR}/usr/lib/libavory-native-compat.so"
MUSL_CC="${MUSL_CC:-x86_64-linux-musl-gcc}"
if ! command -v "${MUSL_CC}" >/dev/null 2>&1; then
    echo "[!] ${MUSL_CC} is required to build the native compatibility shim." >&2
    exit 1
fi
cat > "${NATIVE_COMPAT_SRC}" <<'NATIVE_COMPAT_EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*dlopen_fn)(const char *, int);
static dlopen_fn next_dlopen;

__attribute__((constructor))
static void resolve_dlopen(void) {
    next_dlopen = (dlopen_fn)dlsym(RTLD_NEXT, "dlopen");
}

void *dlopen(const char *path, int mode) {
    if (!next_dlopen)
        next_dlopen = (dlopen_fn)dlsym(RTLD_NEXT, "dlopen");
    if (!next_dlopen)
        return NULL;
    return next_dlopen(path, (mode & ~RTLD_LAZY) | RTLD_NOW);
}

int __vsnprintf_chk(char *dst, size_t size, int flag, size_t dst_size,
                    const char *format, va_list args) {
    (void)flag;
    if (size > dst_size)
        abort();
    return vsnprintf(dst, size, format, args);
}

int __snprintf_chk(char *dst, size_t size, int flag, size_t dst_size,
                   const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = __vsnprintf_chk(dst, size, flag, dst_size, format, args);
    va_end(args);
    return result;
}

int __vfprintf_chk(FILE *stream, int flag, const char *format, va_list args) {
    (void)flag;
    return vfprintf(stream, format, args);
}

int __fprintf_chk(FILE *stream, int flag, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = __vfprintf_chk(stream, flag, format, args);
    va_end(args);
    return result;
}

void *__memmove_chk(void *dst, const void *src, size_t size, size_t dst_size) {
    if (size > dst_size)
        abort();
    return memmove(dst, src, size);
}
NATIVE_COMPAT_EOF
"${MUSL_CC}" -shared -fPIC -O2 -Wl,-z,now \
    -o "${NATIVE_COMPAT_LIB}" "${NATIVE_COMPAT_SRC}" -ldl
echo "[+] Built native compatibility shim → /usr/lib/libavory-native-compat.so"

# ─────────────────────────────────────────────────────────────────────────────
# 2. Stage minecraft.jar
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [2/4] Staging Minecraft ${MC_VERSION} client JAR ==="

jar_sha1() { sha1sum "$1" 2>/dev/null | awk '{print $1}'; }
jar_is_target() { [ "$(jar_sha1 "$1")" = "${MC_CLIENT_SHA1}" ]; }

MC_JAR_CACHE="${BUILD_DIR}/mc1165-client.jar"
MC_JAR_SRC=""
if [ -n "${MC_JAR_OVERRIDE}" ]; then
    [ -f "${MC_JAR_OVERRIDE}" ] || {
        echo "[!] --jar file not found: ${MC_JAR_OVERRIDE}" >&2
        exit 1
    }
    MC_JAR_SRC="${MC_JAR_OVERRIDE}"
    jar_is_target "${MC_JAR_SRC}" ||
        echo "[!] Warning: --jar SHA-1 differs from the official ${MC_VERSION} client JAR; continuing."
elif jar_is_target "${MC_JAR_CACHE}"; then
    MC_JAR_SRC="${MC_JAR_CACHE}"
elif jar_is_target "${ROOT_DIR}/assets/minecraft.jar"; then
    # A previously downloaded 1.16.5 client JAR kept in assets/ (gitignored).
    cp -f "${ROOT_DIR}/assets/minecraft.jar" "${MC_JAR_CACHE}"
    MC_JAR_SRC="${MC_JAR_CACHE}"
else
    echo "[*] Downloading Minecraft ${MC_VERSION} client JAR (link from mcversions.net)..."
    rm -f "${MC_JAR_CACHE}"
    dl "${MC_CLIENT_URL}" "${MC_JAR_CACHE}"
    jar_is_target "${MC_JAR_CACHE}" || {
        echo "[!] Client JAR SHA-1 verification failed (expected ${MC_CLIENT_SHA1})." >&2
        rm -f "${MC_JAR_CACHE}"
        exit 1
    }
    MC_JAR_SRC="${MC_JAR_CACHE}"
fi

mkdir -p "${ROOTFS_DIR}/opt/minecraft"
cp "${MC_JAR_SRC}" "${ROOTFS_DIR}/opt/minecraft/minecraft.jar"
echo "[+] Staged minecraft.jar (${MC_VERSION}) → /opt/minecraft/"

# ─────────────────────────────────────────────────────────────────────────────
# 3. Launcher script + desktop entries
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [3/4] Writing launcher + desktop entries ==="

mkdir -p "${ROOTFS_DIR}/usr/bin"
cat > "${ROOTFS_DIR}/usr/bin/minecraft" << LAUNCHER_EOF
#!/bin/sh
# Minecraft 1.16.5 launcher for AvoryOS

export DISPLAY="\${DISPLAY:-:0}"

# Software GL (Max Performance llvmpipe)
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export LP_NUM_THREADS="\${LP_NUM_THREADS:-4}"
export LP_PERF=no_linear,no_mipmap
export MESA_NO_DITHER=1
export vblank_mode=0
export MESA_SHADER_CACHE_DISABLE=true
export MESA_GLSL_CACHE_DISABLE=true
# Minecraft 1.16.5 renders via Blaze3D, which needs OpenGL 3.2 core / GLSL 150.
# Do not pin MESA_GL_VERSION_OVERRIDE or MESA_GLSL_VERSION_OVERRIDE here:
# llvmpipe already advertises OpenGL 4.5, and forcing 2.1/120 breaks shaders.
# AvoryOS exposes its HDA/AC97 audio through the OSS-compatible /dev/dsp.
# ALSOFT_CONF must be a config-file path, not inline configuration text.
# Restrict OpenAL Soft to OSS so it does not probe unsupported host backends.
export ALSOFT_DRIVERS="\${ALSOFT_DRIVERS:-oss}"

JAVA_HOME="${JAVA_HOME_GUEST}"
export PATH="\${JAVA_HOME}/bin:\${PATH}"
# The natives directory must be first: musl resolves bare sonames such as
# libopenal.so through LD_LIBRARY_PATH when LWJGL/GLFW dlopen() them.
export LD_LIBRARY_PATH="/opt/minecraft/natives:\${JAVA_HOME}/lib:\${JAVA_HOME}/lib/server:/usr/lib:/lib:\${LD_LIBRARY_PATH:-}"

MC_PRELOAD="/usr/lib/libavory-native-compat.so"
[ -f /usr/lib/libjemalloc.so.2 ] && MC_PRELOAD="\${MC_PRELOAD}:/usr/lib/libjemalloc.so.2"

MC_USER="\${MC_USER:-${MC_USERNAME}}"
MC_RAM="\${MC_RAM:-1024m}"
MC_HOME="\${HOME}/.minecraft"
mkdir -p "\${MC_HOME}/saves" "\${MC_HOME}/resourcepacks"

# Build classpath: minecraft.jar + all libs
MC_CP="/opt/minecraft/minecraft.jar"
for jar in /opt/minecraft/libs/*.jar; do
    MC_CP="\${MC_CP}:\${jar}"
done

# Write 1.16.5-compatible options.txt (sound enabled); unknown keys are
# ignored by the game, so only well-known keys are written here.
cat > "\${MC_HOME}/options.txt" << 'OPT_EOF'
invertYMouse:false
mouseSensitivity:0.5
fov:0.0
gamma:1.0
renderDistance:2
guiScale:0
particles:2
bobView:false
anaglyph3d:false
clouds:0
fancyGraphics:true
ambientocclusion:false
useVbo:true
mipmapLevels:0
entityShadows:false
showCape:false
difficulty:1
resourcePacks:[]
lang:en_us
chatVisibility:0
chatColors:true
chatLinks:false
chatLinksPrompt:false
chatOpacity:1.0
snooperEnabled:false
fullscreen:false
enableVsync:false
hideServerAddress:false
advancedItemTooltips:false
pauseOnLostFocus:true
touchscreen:false
overrideWidth:0
overrideHeight:0
heldItemTooltips:true
chatHeightFocused:1.0
chatHeightUnfocused:0.44366196
chatScale:1.0
chatWidth:1.0
autoJump:false
forceUnicodeFont:false
soundCategory_master:1.0
soundCategory_music:0.5
soundCategory_record:1.0
soundCategory_weather:1.0
soundCategory_block:1.0
soundCategory_hostile:1.0
soundCategory_neutral:1.0
soundCategory_player:1.0
soundCategory_ambient:1.0
soundCategory_voice:1.0
OPT_EOF

echo "[minecraft] Starting Minecraft ${MC_VERSION} as '\${MC_USER}' (Heap: \${MC_RAM}, Max FPS Profile)..."
# Resolve all PLT entries while loading. AvoryOS's current runtime loader can
# leave lazy JUMP_SLOT trampolines in dlopen'd libraries such as OpenAL
# unrebased (for example, jumping to 0x86ae instead of base+0x86ae).
LD_BIND_NOW=1 \
LD_PRELOAD="\${MC_PRELOAD}\${LD_PRELOAD:+:\$LD_PRELOAD}" \
MALLOC_CONF="background_thread:false,dirty_decay_ms:5000,muzzy_decay_ms:5000" \
exec "\${JAVA_HOME}/bin/java" \
    -server \
    -Xms256m -Xmx"\${MC_RAM}" \
    -Xss1m \
    -XX:+UseParallelGC \
    -XX:ParallelGCThreads=2 \
    -XX:CICompilerCount=2 \
    -XX:+TieredCompilation \
    -XX:ReservedCodeCacheSize=48m \
    -XX:+DoEscapeAnalysis \
    -XX:+EliminateLocks \
    -XX:-UsePerfData \
    -XX:+UnlockDiagnosticVMOptions \
    -XX:-ImplicitNullChecks \
    --add-opens java.base/java.nio=ALL-UNNAMED \
    --add-opens java.base/java.lang=ALL-UNNAMED \
    --add-opens java.base/java.lang.reflect=ALL-UNNAMED \
    --add-opens java.base/java.util=ALL-UNNAMED \
    --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
    --add-exports java.base/jdk.internal.misc=ALL-UNNAMED \
    --add-exports java.base/sun.security.action=ALL-UNNAMED \
    -Dio.netty.eventLoopThreads=2 \
    -Dhttp.keepAlive=false \
    -Dsun.net.client.defaultConnectTimeout=3000 \
    -Dsun.net.client.defaultReadTimeout=3000 \
    -Djdk.lang.Process.launchMechanism=posix_spawn \
    -Dminecraft.launcher.brand=AvoryOS \
    -Dminecraft.launcher.version=${MC_VERSION} \
    -Dorg.lwjgl.librarypath=/opt/minecraft/natives \
    -Dnet.java.games.input.librarypath=/opt/minecraft/natives \
    -Djava.library.path=/opt/minecraft/natives \
    -Dos.name=Linux \
    -cp "\${MC_CP}" \
    net.minecraft.client.main.Main \
    --username "\${MC_USER}" \
    --version "${MC_VERSION}" \
    --gameDir "\${MC_HOME}" \
    --assetsDir "/opt/minecraft/assets" \
    --assetIndex "${MC_ASSET_INDEX}" \
    --uuid "00000000-0000-0000-0000-000000000000" \
    --accessToken "0" \
    --userType "legacy" \
    --versionType "release"
LAUNCHER_EOF

chmod +x "${ROOTFS_DIR}/usr/bin/minecraft"
echo "[+] Wrote /usr/bin/minecraft"

# .desktop file
mkdir -p "${ROOTFS_DIR}/usr/share/applications"
# Drop the entry written by pre-1.16.5 setups.
rm -f "${ROOTFS_DIR}/usr/share/applications/minecraft-alpha.desktop"
cat > "${ROOTFS_DIR}/usr/share/applications/minecraft-${MC_VERSION}.desktop" << DESKTOP_EOF
[Desktop Entry]
Type=Application
Name=Minecraft ${MC_VERSION}
GenericName=Block Game
Comment=Minecraft ${MC_VERSION} Java Edition
Exec=minecraft
Icon=minecraft-alpha
Terminal=false
Categories=Game;
Keywords=minecraft;blocks;survival;
DESKTOP_EOF
echo "[+] Wrote minecraft-${MC_VERSION}.desktop"

# Openbox menu entry
OPENBOX_MENU="${ROOTFS_DIR}/etc/xdg/openbox/menu.xml"
if [ -f "${OPENBOX_MENU}" ]; then
    if grep -q 'st -e minecraft' "${OPENBOX_MENU}"; then
        sed -i "s|<item label=\"Minecraft [^\"]*\">|<item label=\"Minecraft ${MC_VERSION}\">|" \
            "${OPENBOX_MENU}"
        echo "[+] Updated Openbox menu.xml → Minecraft ${MC_VERSION}"
    elif ! grep -q 'minecraft' "${OPENBOX_MENU}"; then
        sed -i "s|<separator/>|<item label=\"Minecraft ${MC_VERSION}\">\n      <action name=\"Execute\"><execute>st -e minecraft</execute></action>\n    </item>\n    <separator/>|" \
            "${OPENBOX_MENU}"
        echo "[+] Patched Openbox menu.xml"
    fi
fi

# Java PATH profile
cat > "${ROOTFS_DIR}/etc/profile.d/java_avoryos.sh" << ENV_EOF
export JAVA_HOME="${JAVA_HOME_GUEST}"
export PATH="\${JAVA_HOME}/bin:\${PATH}"
ENV_EOF
chmod +x "${ROOTFS_DIR}/etc/profile.d/java_avoryos.sh"
echo "[+] Wrote /etc/profile.d/java_avoryos.sh"

# ─────────────────────────────────────────────────────────────────────────────
# Optional disk expansion
# ─────────────────────────────────────────────────────────────────────────────
if [ "${EXPAND_DISK}" = "1" ]; then
    CURRENT_SIZE=$(stat -c%s "${DISK_IMG}")
    TARGET_SIZE=$((8 * 1024 * 1024 * 1024))
    if [ "${CURRENT_SIZE}" -ge "${TARGET_SIZE}" ]; then
        echo "[*] disk.img already >= 8 GiB. Skipping."
    else
        echo "[*] Expanding disk.img to 8 GiB..."
        truncate -s 8G "${DISK_IMG}"
        PART_IMG="${BUILD_DIR}/part_expand.img"
        dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none
        e2fsck -f -y "${PART_IMG}" || true
        resize2fs "${PART_IMG}"
        dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none
        rm -f "${PART_IMG}"
        echo "[+] disk.img expanded."
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# 4. Inject into disk.img
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [4/4] Injecting into disk.img ==="

PART_IMG="${BUILD_DIR}/part_mc.img"
dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none
"${POPULATE_SCRIPT}" "${PART_IMG}" "${ROOTFS_DIR}" "/"
dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none
rm -f "${PART_IMG}"

echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║  [SUCCESS] Minecraft 1.16.5 is ready in AvoryOS!                 ║"
echo "╠══════════════════════════════════════════════════════════════════╣"
echo "║  Boot AvoryOS → open a terminal → type:  minecraft               ║"
echo "║  Custom username:  MC_USER=YourName minecraft                    ║"
echo "║  Custom RAM:       MC_RAM=1536m minecraft                        ║"
echo "║  Assets incl. 2,336 OGG sounds staged in /opt/minecraft/assets   ║"
echo "╚══════════════════════════════════════════════════════════════════╝"