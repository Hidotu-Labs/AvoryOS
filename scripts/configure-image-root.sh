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
# Keep the prompt entirely printable while showing the active user, hostname,
# and working directory.  Bash expands these prompt escapes for each command.
PS1='\u@\h:\w\$ '
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
# Preserve the home directory supplied by SDDM or the login account. Root's
# passwd entry already selects /; forcing it here breaks every desktop user.
export HOME="${HOME:-/}"
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
127.0.1.1 AvoryOS
EOF
# Resolve local names before trying the packaged systemd NSS services or DNS.
# In particular, sudo resolves the hostname before showing its password prompt.
if [ -f "${ROOTFS_DIR}/etc/nsswitch.conf" ]; then
    awk '$1 == "hosts:" {
        printf "hosts: files"
        for (i = 2; i <= NF; i++) if ($i != "files") printf " %s", $i
        print ""
        next
    } { print }' "${ROOTFS_DIR}/etc/nsswitch.conf" > "${CONFIG_DIR}/nsswitch.conf"
fi
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
{
  "general": {"detectVersion": false},
  "logo": {"source": "/fastfetch/logo.txt", "type": "auto"},
  "modules": [
    "title", "separator", "os", "kernel", "uptime", "packages",
    {"type": "shell", "format": "bash"},
    "de", "wm", "terminal", "cursor", "cpu",
    {"type": "opengl", "key": "GPU", "library": "glx", "format": "{renderer}"},
    "memory", "swap", "disk",
    {"type": "localip", "key": "Local IP", "showIpv6": false},
    "locale", "break", "colors"
  ]
}
EOF

if [ -f "${ROOT_DIR}/config/kwinoutputconfig.json" ]; then
    cp -f "${ROOT_DIR}/config/kwinoutputconfig.json" "${CONFIG_DIR}/kwinoutputconfig.json"
fi

AVORY_UID="$(awk -F: '$1 == "avory" { print $3; exit }' "${ROOTFS_DIR}/etc/passwd")"
AVORY_GID="$(awk -F: '$1 == "avory" { print $4; exit }' "${ROOTFS_DIR}/etc/passwd")"
if [[ ! "${AVORY_UID}" =~ ^[0-9]+$ || ! "${AVORY_GID}" =~ ^[0-9]+$ ]] ||
   [ ! -d "${ROOTFS_DIR}/home/avory" ]; then
    echo "The target root must have an Avory user with a home directory." >&2
    exit 1
fi

# Ensure default KWin output layout disables the headless Virtual-1 output
# while keeping HDMI-A-1 active across all accounts.
# Fastfetch prefers each account's XDG config over the shared /.config file,
# so seed the existing accounts and /etc/skel with the same GLX renderer view.
# The image builder reserves /bin as a real directory for AvoryOS launchers,
# so restore the essential usrmerge links needed by scripts and login shells.
{
    cat <<EOF
rm bin/sh
symlink bin/sh /usr/bin/bash
rm bin/bash
symlink bin/bash /usr/bin/bash
rm bin/env
symlink bin/env /usr/bin/env
rm bin/rm
symlink bin/rm /usr/bin/rm
mkdir etc
rm etc/mtab
symlink etc/mtab /proc/mounts
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
mkdir root/.config/fastfetch
rm root/.config/fastfetch/config.jsonc
write ${CONFIG_DIR}/fastfetch.jsonc root/.config/fastfetch/config.jsonc
mkdir home/avory/.config/fastfetch
rm home/avory/.config/fastfetch/config.jsonc
write ${CONFIG_DIR}/fastfetch.jsonc home/avory/.config/fastfetch/config.jsonc
set_inode_field home/avory/.config/fastfetch uid 1000
set_inode_field home/avory/.config/fastfetch gid 1000
set_inode_field home/avory/.config/fastfetch mode 040755
set_inode_field home/avory/.config/fastfetch/config.jsonc uid 1000
set_inode_field home/avory/.config/fastfetch/config.jsonc gid 1000
set_inode_field home/avory/.config/fastfetch/config.jsonc mode 0100644
mkdir etc/skel/.config
mkdir etc/skel/.config/fastfetch
rm etc/skel/.config/fastfetch/config.jsonc
write ${CONFIG_DIR}/fastfetch.jsonc etc/skel/.config/fastfetch/config.jsonc
rm fastfetch/config.jsonc
write ${CONFIG_DIR}/fastfetch.jsonc fastfetch/config.jsonc
rm fastfetch/logo.txt
write ${ROOT_DIR}/assets/ascii-art.txt fastfetch/logo.txt
EOF
    if [ -f "${CONFIG_DIR}/nsswitch.conf" ]; then
        printf 'rm etc/nsswitch.conf\nwrite %s etc/nsswitch.conf\n' "${CONFIG_DIR}/nsswitch.conf"
    fi
    # Rootless package extraction cannot retain root ownership and may drop
    # setuid. Restore sudo's installed metadata inside the image, where it
    # does not grant privileges to the host build account.
    if [ -f "${ROOTFS_DIR}/usr/bin/sudo" ]; then
        printf '%s\n' \
            'set_inode_field usr/bin/sudo uid 0' \
            'set_inode_field usr/bin/sudo gid 0' \
            'set_inode_field usr/bin/sudo mode 0104755' \
            'set_inode_field etc/sudoers uid 0' \
            'set_inode_field etc/sudoers gid 0' \
            'set_inode_field etc/sudoers mode 0100440'
    fi
    while IFS= read -r -d '' entry; do
        image_path="${entry#"${ROOTFS_DIR}/"}"
        printf 'set_inode_field "%s" uid %s\n' "${image_path}" "${AVORY_UID}"
        printf 'set_inode_field "%s" gid %s\n' "${image_path}" "${AVORY_GID}"
    done < <(find "${ROOTFS_DIR}/home/avory" -print0)
} | debugfs -w "${IMAGE}" >/dev/null 2>&1 || true

for home_path in home/avory home/avory/.config home/avory/.config/kwinrc; do
    details="$(debugfs -R "stat /${home_path}" "${IMAGE}" 2>&1 || true)"
    if ! grep -Eq "User:[[:space:]]+${AVORY_UID}[[:space:]]+Group:[[:space:]]+${AVORY_GID}([[:space:]]|$)" <<<"${details}"; then
        echo "Could not assign /${home_path} to Avory (${AVORY_UID}:${AVORY_GID}):" >&2
        printf '%s\n' "${details}" >&2
        exit 1
    fi
done

if [ -f "${ROOTFS_DIR}/usr/bin/sudo" ]; then
    for entry in 'usr/bin/sudo 04755' 'etc/sudoers 0440'; do
        read -r image_path expected_mode <<<"${entry}"
        details="$(debugfs -R "stat /${image_path}" "${IMAGE}" 2>&1 || true)"
        if ! grep -Eq "User:[[:space:]]+0[[:space:]]+Group:[[:space:]]+0([[:space:]]|$)" <<<"${details}" ||
           ! grep -Eq "Mode:[[:space:]]+${expected_mode}([[:space:]]|$)" <<<"${details}"; then
            echo "Could not restore /${image_path} root ownership and mode ${expected_mode}:" >&2
            printf '%s\n' "${details}" >&2
            exit 1
        fi
    done
fi
