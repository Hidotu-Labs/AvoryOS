#!/usr/bin/env bash
# Prepare pacman alongside Axon without changing either package manager.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOTFS_DIR="$(realpath "${1:?usage: configure-pacman.sh ROOTFS_DIR}")"
[ -f "${ROOTFS_DIR}/etc/pacman.conf" ] || exit 0

# The packages ship trust sources and symlinks, but the generated bundles
# require a hook which rootless bootstrap may not have executed. Generate
# them from the target's own trust store before any HTTPS package download.
if [ ! -x "${ROOTFS_DIR}/usr/bin/update-ca-trust" ]; then
    echo "Missing update-ca-trust in the target root." >&2
    exit 1
fi
echo "Generating the target TLS certificate bundles..."
unshare -r chroot "${ROOTFS_DIR}" /usr/bin/update-ca-trust extract
python3 - "${ROOTFS_DIR}/etc/ssl/certs/ca-certificates.crt" <<'PY'
import ssl
import sys
context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.load_verify_locations(cafile=sys.argv[1])
if not context.cert_store_stats()['x509_ca']:
    raise SystemExit('The target TLS certificate bundle contains no CA certificates')
print(f"TLS trust store ready: {context.cert_store_stats()['x509_ca']} CA certificates")
PY

# Pacman's CheckSpace needs a readable mount table. Use the kernel's global
# mount listing directly instead of the package's relative /proc/self link.
ln -sfn /proc/mounts "${ROOTFS_DIR}/etc/mtab"

# Axon's bootstrap and later Arch packages can leave two records for the
# same package. Retain the most recently installed record, not simply the
# highest version (an intentional downgrade is valid). Archive stale metadata
# outside pacman's local database; do not remove any installed payload files.
python3 - "${ROOTFS_DIR}" <<'PY'
from collections import defaultdict
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
local = root/'var/lib/pacman/local'
backup = root/'var/lib/avoryos/pacman-local-backups'

def field(desc, name):
    fields = desc.read_text().splitlines()
    marker = f'%{name}%'
    return fields[fields.index(marker) + 1] if marker in fields else ''

def files_section(record):
    """Return (paths in %FILES%, lines of any later sections)."""
    lines = (record/'files').read_text().splitlines()
    paths, rest, section = [], [], None
    for line in lines:
        if line.startswith('%') and line.endswith('%'):
            section = line
        if section == '%FILES%':
            if line and line != '%FILES%':
                paths.append(line)
        else:
            rest.append(line)
    return paths, rest

def adopt_leftovers(keep, stale_records):
    """A replaced record may have installed paths the kept one does not list
    (Arch bash ships usr/bin/sh, usr/bin/rbash and bash.info.gz; Axon's bash
    of the same version does not). Those files stay on disk, so dropping the
    record orphans them and every later pacman upgrade of the package fails
    with "exists in filesystem". Give still-present paths that no other
    installed package owns to the kept record."""
    owned = set()
    for other in local.iterdir():
        if other != keep and other.is_dir() and (other/'files').is_file():
            owned.update(files_section(other)[0])
    kept, rest = files_section(keep)
    adopted = set()
    for stale in stale_records:
        if not (stale/'files').is_file():
            continue
        for path in files_section(stale)[0]:
            if path not in kept and path not in owned and \
               os.path.lexists(root/path.rstrip('/')):
                adopted.add(path)
    if adopted:
        body = '\n'.join(['%FILES%', *sorted(set(kept) | adopted)]) + '\n\n'
        # AvoryOS writes its own etc/ files (bash.bashrc, profile...). List
        # adopted config files as modified backups (hash never matches) so a
        # later pacman upgrade keeps them and installs a .pacnew instead.
        configs = sorted(p for p in adopted if p.startswith('etc/')
                         and not p.endswith('/') and (root/p).is_file())
        if configs:
            backed = {l.split('\t')[0] for l in rest if '\t' in l}
            entries = [f'{p}\t{"0" * 32}' for p in configs if p not in backed]
            if '%BACKUP%' not in rest:
                rest = ['%BACKUP%'] + rest
            i = rest.index('%BACKUP%') + 1
            rest[i:i] = entries
        tail = '\n'.join(rest).strip('\n')
        (keep/'files').write_text(body + (tail + '\n\n' if tail else ''))
        print(f'{keep.name}: adopted {len(adopted)} path(s) from the replaced record')

packages = defaultdict(list)
for entry in local.iterdir():
    if not entry.is_dir() or entry.is_symlink():
        continue
    desc = entry/'desc'
    if not desc.is_file():
        continue
    name = field(desc, 'NAME')
    if not name or not field(desc, 'INSTALLDATE').isdigit():
        raise SystemExit(f'Invalid installed package metadata: {desc}')
    packages[name].append((int(field(desc, 'INSTALLDATE')), desc.stat().st_mtime_ns, entry))

for name, entries in packages.items():
    if len(entries) < 2:
        continue
    entries.sort(key=lambda item: item[:2])
    if entries[-1][:2] == entries[-2][:2]:
        raise SystemExit(f'Ambiguous installation order for duplicate package {name}')
    keep = entries[-1][2]
    if not (keep/'files').is_file():
        raise SystemExit(f'Incomplete installed package metadata: {keep}')
    adopt_leftovers(keep, [e[2] for e in entries[:-1]])
    backup.mkdir(parents=True, exist_ok=True)
    for _, _, stale in entries[:-1]:
        destination = Path(tempfile.mkdtemp(prefix=stale.name+'-', dir=backup))
        shutil.move(str(stale), destination/stale.name)
        print(f'Archived stale pacman record {stale.name}; keeping {keep.name}')

current = {name: entries[-1][2] for name, entries in packages.items()
           if entries[-1][2].is_dir() and (entries[-1][2]/'files').is_file()}

# Repair roots whose duplicates were archived before adoption existed.
if backup.is_dir():
    for archived in backup.glob('*/*'):
        if archived.is_dir() and (archived/'desc').is_file():
            keep = current.get(field(archived/'desc', 'NAME'))
            if keep:
                adopt_leftovers(keep, [archived])

# A record can also be replaced in place (Arch bash 5.3.20 overwritten by
# Axon's bash 5.3.15) with no copy of the old file list left. The old package
# archive is still in the cache, so take that version's file list from it.
with tempfile.TemporaryDirectory() as scratch:
    for cache in ('var/cache/axon/pkg', 'var/cache/pacman/pkg'):
        for archive in sorted((root/cache).glob('*.pkg.tar.*')):
            if archive.name.endswith('.sig'):
                continue
            stem = archive.name.split('.pkg.tar.')[0]
            name = stem.rsplit('-', 3)[0]
            keep = current.get(name)
            if not keep or stem.startswith(keep.name + '-'):
                continue
            listing = subprocess.run([shutil.which('bsdtar') or 'tar', '-tf', str(archive)],
                                     capture_output=True, text=True)
            if listing.returncode:
                print(f'Warning: could not list {archive}', file=sys.stderr)
                continue
            record = Path(scratch)/stem
            record.mkdir()
            paths = [p for p in listing.stdout.splitlines() if p and not p.startswith('.')]
            (record/'files').write_text('%FILES%\n' + '\n'.join(paths) + '\n')
            adopt_leftovers(keep, [record])
PY

# Package sysusers hooks are not guaranteed to run during rootless bootstrap.
# Allocate unused system IDs instead of assuming a distro-specific number.
free_id() {
    awk -F: '{ used[$3] = 1 } END {
        for (id = 999; id >= 101; id--) if (!used[id]) { print id; exit }
        exit 1
    }' "$1"
}
if ! grep -q '^alpm:' "${ROOTFS_DIR}/etc/group"; then
    gid=$(free_id "${ROOTFS_DIR}/etc/group")
    printf 'alpm:x:%s:\n' "$gid" >> "${ROOTFS_DIR}/etc/group"
fi
if ! grep -q '^alpm:' "${ROOTFS_DIR}/etc/passwd"; then
    uid=$(free_id "${ROOTFS_DIR}/etc/passwd")
    gid=$(awk -F: '$1 == "alpm" { print $3; exit }' "${ROOTFS_DIR}/etc/group")
    printf 'alpm:!:%s:%s:Arch Linux Package Management:/:/usr/bin/nologin\n' \
        "$uid" "$gid" >> "${ROOTFS_DIR}/etc/passwd"
fi

install -d "${ROOTFS_DIR}/var/lib/pacman/sync" "${ROOTFS_DIR}/var/cache/pacman/pkg" \
    "${ROOTFS_DIR}/etc/pacman.d"
install -m 0644 "${ROOT_DIR}/config/axon/mirrorlist" "${ROOTFS_DIR}/etc/pacman.d/mirrorlist"
# Keep pacman able to install the same i686 runtime packages Axon provisions
# for 32-bit applications. The multilib database is already part of Axon's
# synchronized repository snapshot below.
if ! grep -q '^\[multilib\]$' "${ROOTFS_DIR}/etc/pacman.conf"; then
    cat >> "${ROOTFS_DIR}/etc/pacman.conf" <<'EOF'

[multilib]
Include = /etc/pacman.d/mirrorlist
EOF
fi
# Axon downloaded the standard Arch repository archives already. Seed the
# enabled pacman repositories from that exact snapshot, not from another sync.
while IFS= read -r repo; do
    db="${ROOTFS_DIR}/var/lib/axon/sync/${repo}.db"
    if [ ! -s "$db" ] || ! tar -tf "$db" > /dev/null; then
        echo "Missing or invalid Axon repository database: $db" >&2
        exit 1
    fi
    install -m 0644 "$db" "${ROOTFS_DIR}/var/lib/pacman/sync/${repo}.db"
done < <(awk '/^[[:space:]]*\[[^]]+\]/ {
    gsub(/[[:space:]\[\]]/, ""); if ($0 != "options") print
}' "${ROOTFS_DIR}/etc/pacman.conf")

# AvoryOS has no Landlock implementation. Retain the unprivileged downloader
# and seccomp filtering; only disable the unsupported filesystem sandbox.
if ! grep -q '^DisableSandboxFilesystem[[:space:]]*$' "${ROOTFS_DIR}/etc/pacman.conf"; then
    sed -i '/^\[options\]/a DisableSandboxFilesystem' "${ROOTFS_DIR}/etc/pacman.conf"
fi

keyrings="${ROOTFS_DIR}/usr/share/pacman/keyrings"
keydir="${ROOTFS_DIR}/etc/pacman.d/gnupg"
fingerprint=$(sha256sum "${keyrings}/archlinux.gpg" "${keyrings}/archlinux-trusted" \
    "${keyrings}/archlinux-revoked" | sha256sum | cut -d ' ' -f 1)
if [ ! -s "${keydir}/pubring.gpg" ] || [ ! -s "${keydir}/trustdb.gpg" ] ||
   [ "$(cat "${keydir}/.avory-keyring" 2>/dev/null || true)" != "$fingerprint" ]; then
    echo "Initializing pacman's Arch signing keyring..."
    mkdir -p "${ROOTFS_DIR}/dev"
    for device in null zero random urandom; do
        [ -e "${ROOTFS_DIR}/dev/${device}" ] || touch "${ROOTFS_DIR}/dev/${device}"
    done
    # Use the same rootless mount/chroot approach as Axon's target runtime.
    # No host keyring is imported, and no keyserver/network access is needed.
    unshare -rm /bin/bash -eu -c '
        root=$1
        for device in null zero random urandom; do
            mount --bind "/dev/$device" "$root/dev/$device"
        done
        trap '\''chroot "$root" /usr/bin/gpgconf --kill gpg-agent >/dev/null 2>&1 || true'\'' EXIT
        export PATH=/usr/bin:/usr/sbin:/bin:/sbin HOME=/ LC_ALL=C
        chroot "$root" /usr/bin/pacman-key --init
        chroot "$root" /usr/bin/pacman-key --populate archlinux
    ' avory-pacman "${ROOTFS_DIR}"
    printf '%s\n' "$fingerprint" > "${keydir}/.avory-keyring"
fi
echo "Pacman accounts, mirrors, repository databases and signing keyring are ready."
