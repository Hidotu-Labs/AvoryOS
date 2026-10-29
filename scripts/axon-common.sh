#!/usr/bin/env bash
# Shared Axon bootstrap for AvoryOS rootfs provisioning scripts.

AXON_REPO_URL="${AXON_REPO_URL:-https://github.com/Hidotu-Labs/Axon.git}"
AXON_REF="main"
AXON_SOURCE_DIR="${AXON_SOURCE_DIR:-${ROOT_DIR}/build/axon-src}"
case "${AXON_SOURCE_DIR}" in
    /*) ;;
    *) AXON_SOURCE_DIR="${ROOT_DIR}/${AXON_SOURCE_DIR}" ;;
esac
AXON_MIRRORLIST_FILE="${ROOT_DIR}/config/axon/mirrorlist"

ensure_axon() {
    if [ -n "${AXON_BIN:-}" ]; then
        case "${AXON_BIN}" in
            */*)
                if [ ! -x "${AXON_BIN}" ]; then
                    echo "[!] AXON_BIN is not executable: ${AXON_BIN}" >&2
                    return 1
                fi
                AXON_BIN="$(cd "$(dirname "${AXON_BIN}")" && pwd)/$(basename "${AXON_BIN}")"
                ;;
            *)
                AXON_BIN="$(command -v "${AXON_BIN}")" || {
                    echo "[!] AXON_BIN command was not found: ${AXON_BIN}" >&2
                    return 1
                }
                ;;
        esac
        export PATH="$(dirname "${AXON_BIN}"):${PATH}"
        return 0
    fi

    local source_binary="${AXON_SOURCE_DIR}/axon"
    if [ ! -d "${AXON_SOURCE_DIR}/.git" ]; then
        for tool in git; do
            if ! command -v "${tool}" >/dev/null 2>&1; then
                echo "[!] ${tool} is required to build Axon on this host." >&2
                return 1
            fi
        done
        mkdir -p "$(dirname "${AXON_SOURCE_DIR}")"
        if [ -e "${AXON_SOURCE_DIR}" ]; then
            echo "[!] Axon source path exists but is not a Git checkout: ${AXON_SOURCE_DIR}" >&2
            return 1
        fi
        echo "[*] Fetching Axon from ${AXON_REPO_URL} (${AXON_REF})..."
        git clone --depth 1 --branch "${AXON_REF}" \
            "${AXON_REPO_URL}" "${AXON_SOURCE_DIR}"
    else
        local current_ref current_head fetched_head stash_created=0
        current_ref="$(git -C "${AXON_SOURCE_DIR}" branch --show-current 2>/dev/null || true)"
        echo "[*] Checking for the latest Axon on ${AXON_REF}..."
        git -C "${AXON_SOURCE_DIR}" fetch --depth 1 origin "${AXON_REF}"
        current_head="$(git -C "${AXON_SOURCE_DIR}" rev-parse HEAD)"
        fetched_head="$(git -C "${AXON_SOURCE_DIR}" rev-parse FETCH_HEAD)"
        if [ "${current_ref}" != "${AXON_REF}" ] || [ "${current_head}" != "${fetched_head}" ]; then
            echo "[*] Updating managed Axon checkout to ${AXON_REF}..."
            if [ -n "$(git -C "${AXON_SOURCE_DIR}" status --porcelain)" ]; then
                git -C "${AXON_SOURCE_DIR}" stash push --include-untracked \
                    -m "AvoryOS Axon bootstrap update" >/dev/null
                stash_created=1
            fi
            git -C "${AXON_SOURCE_DIR}" switch -C "${AXON_REF}" FETCH_HEAD
            if [ "${stash_created}" -eq 1 ]; then
                git -C "${AXON_SOURCE_DIR}" stash pop
            fi
        fi
    fi

    if [ ! -x "${source_binary}" ]; then
        for tool in make cc pkg-config; do
            if ! command -v "${tool}" >/dev/null 2>&1; then
                echo "[!] ${tool} is required to build Axon on this host." >&2
                return 1
            fi
        done
        if ! pkg-config --exists libarchive libcurl; then
            echo "[!] Axon needs the libarchive and libcurl development libraries (pkg-config modules libarchive and libcurl)." >&2
            return 1
        fi
        echo "[*] Building Axon..."
        make -C "${AXON_SOURCE_DIR}"
    fi

    if [ ! -x "${source_binary}" ]; then
        echo "[!] Axon build did not produce ${source_binary}." >&2
        return 1
    fi
    AXON_BIN="${source_binary}"
    export PATH="$(dirname "${AXON_BIN}"):${PATH}"
}

axon_rootfs_command() {
    local rootfs_dir="$1"
    shift
    AXON_MIRRORLIST="${AXON_MIRRORLIST_FILE}" \
        "${AXON_BIN}" --root "${rootfs_dir}" \
        --mirrorlist "${AXON_MIRRORLIST_FILE}" "$@"
}

axon_rootfs_sync() {
    axon_rootfs_command "$1" upd
}

axon_rootfs_list_installed() {
    local db_dir="$1" entry name
    [ -d "${db_dir}" ] || return 0
    while IFS= read -r -d '' entry; do
        name="$(awk '$0 == "%NAME%" { getline; print; exit }' "${entry}")"
        [ -z "${name}" ] || printf '%s\n' "${name}"
    done < <(find "${db_dir}" -mindepth 2 -maxdepth 2 -type f -name desc -print0)
}

axon_rootfs_bootstrap() {
    local rootfs_dir="$1"
    local package_db="${rootfs_dir}/var/lib/pacman/local"
    local package_name
    local -a before_packages=() after_packages=() new_packages=()

    mapfile -t before_packages < <(axon_rootfs_list_installed "${package_db}" | sort -u)

    # The current Axon CLI bootstraps an empty target with a small transaction,
    # then replays its deferred install scripts once bash is available.
    axon_rootfs_command "${rootfs_dir}" get --needed --noconfirm --keep-cache \
        --no-scriptlets filesystem glibc bash readline ncurses

    install -D -m 0644 "${AXON_MIRRORLIST_FILE}" \
        "${rootfs_dir}/etc/axon/mirrorlist"

    mapfile -t after_packages < <(axon_rootfs_list_installed "${package_db}" | sort -u)
    for package_name in "${after_packages[@]}"; do
        if [[ ! " ${before_packages[*]} " == *" ${package_name} "* ]]; then
            new_packages+=("${package_name}")
        fi
    done
    [ "${#new_packages[@]}" -eq 0 ] && return 0
    if [ ! -x "${rootfs_dir}/bin/bash" ] && [ ! -x "${rootfs_dir}/usr/bin/bash" ]; then
        echo "[!] Axon bootstrap installed no usable target bash in ${rootfs_dir}." >&2
        return 1
    fi
    axon_rootfs_command "${rootfs_dir}" replay-post-install \
        "${new_packages[@]}"
}

axon_rootfs_install_list() {
    local rootfs_dir="$1" package_file="$2"
    shift 2
    local -a packages=()
    mapfile -t packages < <(awk 'NF && $1 !~ /^#/ { print $1 }' "${package_file}")
    if [ "${#packages[@]}" -eq 0 ]; then
        echo "[!] No package names found in ${package_file}." >&2
        return 1
    fi
    # Bash and Coreutils both ship the shared GNU info index. Axon's conflict
    # check treats that intentional overlap as a hard file conflict unless
    # overwrite mode is enabled for the rootfs package transaction.
    axon_rootfs_command "${rootfs_dir}" get --needed --noconfirm --keep-cache \
        --overwrite "$@" "${packages[@]}"
}
