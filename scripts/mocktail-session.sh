#!/bin/sh
# Sourced by the Avory launcher. Keep recovery separate from AppImage updates.
mocktail_quarantine() {
    # ext2/ext4 supports directory renames only within the same parent.
    # Reserve a unique sibling name, remove the empty reservation, then rename
    # the original intact. -T prevents mv from nesting it in a directory.
    session_saved=$(mktemp -d "$1.avory-recovery.XXXXXX") || return 1
    rmdir "$session_saved" || return 1
    mv -T -- "$1" "$session_saved" || return 1
    printf '[mocktail] Cache backup: %s\n' "$session_saved" >&2
    # The recovery tree is an index of backups, not their physical parent.
    mkdir -p "$(dirname "$2")" || return 1
    ln -s "$session_saved" "$2"
}

mocktail_session() {
    session_root=${MOCKTAIL_DATA_ROOT:?}
    session_key=$(printf '%s' "$session_root" | cksum) || exit 1
    session_key=${session_key%% *}
    session_guard="${TMPDIR:-/tmp}/avory-mocktail-$(id -u)-${session_key}.session"
    session_clean="$session_root/.avory-session-clean"
    session_runtime_cache="${MOCKTAIL_CACHE_ROOT:?}/android"

    # /tmp is tmpfs on Avory: a reboot cannot leave a live PID claim behind.
    # flock is currently a kernel no-op, so use atomic mkdir for exclusion.
    if ! mkdir -m 700 "$session_guard" 2>/dev/null; then
        session_pid=
        if [ -r "$session_guard/pid" ]; then
            read -r session_pid < "$session_guard/pid"
        fi
        case "$session_pid" in
            ''|*[!0-9]*)
                echo '[mocktail] Another launcher is starting; retry shortly.' >&2
                exit 1 ;;
        esac
        if kill -0 "$session_pid" 2>/dev/null; then
            echo '[mocktail] Mocktail is already running.' >&2
            exit 1
        fi
        rm -f "$session_guard/pid" || exit 1
        rmdir "$session_guard" || exit 1
        mkdir -m 700 "$session_guard" || exit 1
    fi
    # $$ is the supervising launcher PID.
    printf '%s\n' "$$" > "$session_guard/pid" || exit 1
    session_child=
    trap 'rm -f "$session_guard/pid"; rmdir "$session_guard" 2>/dev/null' EXIT
    trap '[ -z "$session_child" ] || kill -TERM "$session_child" 2>/dev/null' TERM HUP
    trap '[ -z "$session_child" ] || kill -INT "$session_child" 2>/dev/null' INT

    # Also recognize a client started by the old launcher (or directly).
    # comm is renamed to Main/RBX Worker; matching /proc/*/exe avoids both
    # renamed threads and pgrep -f accidentally matching this shell script.
    for session_exe in /proc/[0-9]*/exe; do
        case "$(readlink "$session_exe" 2>/dev/null)" in
            */bin/mocktail|*/shared/bin/mocktail)
                echo '[mocktail] Mocktail is already running.' >&2
                exit 1 ;;
        esac
    done

    # No clean marker also covers the first launch after upgrading an older
    # launcher, whose last run may have been cut off by QEMU/power loss.
    session_recover=0
    if [ ! -f "$session_clean" ] || [ "${MOCKTAIL_RECOVER:-0}" = 1 ]; then
        session_recover=1
    fi
    rm -f "$session_clean" || exit 1
    if [ "$session_recover" -eq 1 ]; then
        session_backup=
        session_appdata="$session_root/android/data/files/appData"
        for session_cache in \
            "$session_root/android/data/cache" \
            "$session_appdata/rbx-storage" \
            "$session_appdata/rbx-storage.db" \
            "$session_appdata/rbx-storage.db-journal" \
            "$session_appdata/rbx-storage.db-wal" \
            "$session_appdata/rbx-storage.db-shm"; do
            [ -e "$session_cache" ] || [ -L "$session_cache" ] || continue
            if [ -z "$session_backup" ]; then
                mkdir -p "$session_root/avory-recovery" || exit 1
                session_backup=$(mktemp -d "$session_root/avory-recovery/session.XXXXXX") || exit 1
            fi
            # Mirror paths in the backup index so names cannot collide.
            session_relative=${session_cache#"$session_root"/}
            mocktail_quarantine "$session_cache" "$session_backup/$session_relative" || exit 1
        done
        if [ -n "$session_backup" ]; then
            printf '[mocktail] Recovering interrupted session; caches saved in %s\n' "$session_backup" >&2
        fi
        # The runtime also stores Android caches under MOCKTAIL_CACHE_ROOT,
        # independently of the shared Android data tree.
        if [ -e "$session_runtime_cache" ] || [ -L "$session_runtime_cache" ]; then
            if [ -z "$session_backup" ]; then
                mkdir -p "$session_root/avory-recovery" || exit 1
                session_backup=$(mktemp -d "$session_root/avory-recovery/session.XXXXXX") || exit 1
                printf '[mocktail] Recovering interrupted session; caches saved in %s\n' "$session_backup" >&2
            fi
            mocktail_quarantine "$session_runtime_cache" "$session_backup/runtime-cache/android" || exit 1
        fi
    fi
    if ! mkdir -p "$session_root/android/data/cache" "$session_runtime_cache"; then
        printf '[mocktail] Cannot prepare Android cache directory: %s\n' "$session_runtime_cache" >&2
        exit 1
    fi
    # Never remove arbitrary *.lock files from payloads or persistent data.
    session_endpoint="${TMPDIR:-/tmp}/mocktail-$(id -u)"
    rm -rf "$session_endpoint" || exit 1

    "$@" <&0 &
    session_child=$!
    # A signal interrupts wait; keep supervising until the child has exited.
    while :; do
        wait "$session_child"
        session_status=$?
        kill -0 "$session_child" 2>/dev/null || break
    done
    if [ "$session_status" -eq 0 ]; then
        : > "$session_clean" || exit 1
    fi
    exit "$session_status"
}
