#!/usr/bin/env bash
# Shared Axon bootstrap for AvoryOS rootfs provisioning scripts.

AXON_REPO_URL="${AXON_REPO_URL:-https://github.com/Hidotu-Labs/Axon.git}"
AXON_REF="${AXON_REF:-codex/axon-rootfs-fixes}"
AXON_SOURCE_DIR="${AXON_SOURCE_DIR:-${ROOT_DIR}/build/axon-src}"
case "${AXON_SOURCE_DIR}" in
    /*) ;;
    *) AXON_SOURCE_DIR="${ROOT_DIR}/${AXON_SOURCE_DIR}" ;;
esac

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
        local current_ref
        current_ref="$(git -C "${AXON_SOURCE_DIR}" branch --show-current 2>/dev/null || true)"
        if [ "${current_ref}" != "${AXON_REF}" ]; then
            echo "[*] Updating managed Axon checkout to ${AXON_REF}..."
            git -C "${AXON_SOURCE_DIR}" fetch --depth 1 origin "${AXON_REF}"
            git -C "${AXON_SOURCE_DIR}" switch -C "${AXON_REF}" FETCH_HEAD
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
