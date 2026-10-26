#!/usr/bin/env bash
# Apply AvoryOS's image-specific shell and release defaults after rootfs copy.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 IMAGE ROOTFS" >&2
    exit 2
fi

IMAGE="$1"
ROOTFS_DIR="$2"
case "${IMAGE}" in /*) ;; *) IMAGE="${ROOT_DIR}/${IMAGE}" ;; esac
case "${ROOTFS_DIR}" in /*) ;; *) ROOTFS_DIR="${ROOT_DIR}/${ROOTFS_DIR}" ;; esac

CONFIG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/avory-image-config.XXXXXX")"
trap 'rm -rf "${CONFIG_DIR}"' EXIT

cat > "${CONFIG_DIR}/bashrc" <<'EOF'
# Keep the prompt entirely printable.  Graphical terminal emulators were
# accepting Bash history recall but drawing it off-screen with the colored
# prompt, even though its ANSI spans were marked for Readline.  A plain prompt
# avoids the width mismatch while retaining the host and working directory.
PS1='Root@AvoryOS:\w# '
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
HOME=/
TERM=xterm-256color
if [ -n "$DISPLAY" ]; then unset TERM_PROGRAM TERM_PROGRAM_VERSION; else export TERM_PROGRAM=vt; fi
export TERM
SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
SSL_CERT_DIR=/etc/ssl/certs
CURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt
export SSL_CERT_FILE SSL_CERT_DIR CURL_CA_BUNDLE
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null && chmod 0700 "$XDG_RUNTIME_DIR" 2>/dev/null
# Root HOME is "/" on AvoryOS: join XDG defaults as ${HOME%/}/... so sessions
# never export //.cache-style paths (WebKitGTK rejects those).
_avory_home="${HOME:-/}"; _avory_home="${_avory_home%/}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-${_avory_home}/.config}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-${_avory_home}/.cache}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-${_avory_home}/.local/share}"
export XDG_STATE_HOME="${XDG_STATE_HOME:-${_avory_home}/.local/state}"
unset _avory_home
EOF

cat > "${CONFIG_DIR}/hosts" <<'EOF'
127.0.0.1 localhost
EOF
cat > "${CONFIG_DIR}/resolv.conf" <<'EOF'
nameserver 10.0.2.3
EOF
cat > "${CONFIG_DIR}/os-release" <<'EOF'
NAME="AvoryOS"
ID=avoryos
VERSION="2.5.0 Beta"
VERSION_ID=2.5.0-beta
PRETTY_NAME="AvoryOS 2.5.0 Beta x86_64"
HOME_URL="https://github.com/Hidotu-Labs/AvoryOS"
EOF
cat > "${CONFIG_DIR}/fastfetch.jsonc" <<'EOF'
{"general": {"detectVersion": false}, "logo": {"source": "/fastfetch/logo.txt", "type": "auto"}, "modules": ["title", "separator", "os", "kernel", "uptime", "packages", {"type": "shell", "format": "bash"}, "de", "wm", "terminal", "cursor", "cpu", "gpu", "memory", "swap", "disk", {"type": "localip", "key": "Local IP", "showIpv6": false}, "locale", "break", "colors"]}
EOF

if [ -f "${ROOT_DIR}/config/kwinoutputconfig.json" ]; then
    cp -f "${ROOT_DIR}/config/kwinoutputconfig.json" "${CONFIG_DIR}/kwinoutputconfig.json"
fi

# Ensure default KWin output layout disables the headless Virtual-1 output
# while keeping HDMI-A-1 active across all accounts.
{
    cat <<EOF
mkdir bin
rm bin/sh
symlink bin/sh ../usr/bin/bash
rm bin/bash
symlink bin/bash ../usr/bin/bash
rm bin/env
symlink bin/env ../usr/bin/env
mkdir etc
rm etc/resolv.conf
write ${CONFIG_DIR}/resolv.conf etc/resolv.conf
rm etc/hosts
write ${CONFIG_DIR}/hosts etc/hosts
mkdir etc/ssl
mkdir etc/ssl/certs
rm etc/os-release
write ${CONFIG_DIR}/os-release etc/os-release
rm usr/lib/os-release
write ${CONFIG_DIR}/os-release usr/lib/os-release
rm .bashrc
write ${CONFIG_DIR}/bashrc .bashrc
rm etc/profile
write ${CONFIG_DIR}/bashrc etc/profile
rm etc/bash.bashrc
write ${CONFIG_DIR}/bashrc etc/bash.bashrc
mkdir .config
rm .config/kwinoutputconfig.json
write ${CONFIG_DIR}/kwinoutputconfig.json .config/kwinoutputconfig.json
mkdir root
mkdir root/.config
rm root/.config/kwinoutputconfig.json
write ${CONFIG_DIR}/kwinoutputconfig.json root/.config/kwinoutputconfig.json
mkdir home
mkdir home/avory
mkdir home/avory/.config
rm home/avory/.config/kwinoutputconfig.json
write ${CONFIG_DIR}/kwinoutputconfig.json home/avory/.config/kwinoutputconfig.json
mkdir etc/skel
mkdir etc/skel/.config
rm etc/skel/.config/kwinoutputconfig.json
write ${CONFIG_DIR}/kwinoutputconfig.json etc/skel/.config/kwinoutputconfig.json
mkdir .config/fastfetch
mkdir fastfetch
rm .config/fastfetch/config.jsonc
write ${CONFIG_DIR}/fastfetch.jsonc .config/fastfetch/config.jsonc
rm fastfetch/config.jsonc
write ${CONFIG_DIR}/fastfetch.jsonc fastfetch/config.jsonc
rm fastfetch/logo.txt
write ${ROOT_DIR}/assets/ascii-art.txt fastfetch/logo.txt
EOF
    if [ -f "${ROOTFS_DIR}/etc/ssl/certs/ca-certificates.crt" ]; then
        printf 'rm etc/ssl/certs/ca-certificates.crt\n'
        printf 'write %s etc/ssl/certs/ca-certificates.crt\n' \
            "${ROOTFS_DIR}/etc/ssl/certs/ca-certificates.crt"
    fi
} | debugfs -w "${IMAGE}" >/dev/null 2>&1 || true
