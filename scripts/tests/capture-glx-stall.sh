#!/bin/sh
# Run from a second xterm while the original GLX window is stalled.
set -eu

report=$(mktemp /tmp/glxstall.XXXXXX.txt)
printf 'Collecting samples in %s\n' "$report"
exec 3>&1
exec > "$report" 2>&1

capture=$(mktemp -d)
trap 'rm -f -- "$capture/before" "$capture/after" "$capture/ranked"; rmdir -- "$capture"' EXIT

snapshot() {
    for thread in /proc/[0-9]*/task/[0-9]*; do
        if IFS= read -r stat 2>/dev/null < "$thread/stat"; then
            printf '%s\t%s\n' "$thread" "$stat"
        fi
    done
}

printf '=== Kernel ===\n'
uname -rv
printf '\n=== Display PCI devices ===\n'
for device in /sys/bus/pci/devices/*; do
    [ -r "$device/class" ] || continue
    IFS= read -r class < "$device/class" || continue
    case "$class" in
        0x03*)
            printf '%s %s:%s\n' "${device##*/}" \
                "$(cat "$device/vendor")" "$(cat "$device/device")"
            ;;
    esac
done

printf '\n=== CPU and interrupts before ===\n'
head -n 3 /proc/stat
cat /proc/interrupts
snapshot > "$capture/before"
sleep 2
snapshot > "$capture/after"
printf '\n=== CPU and interrupts after ===\n'
head -n 3 /proc/stat
cat /proc/interrupts

printf '\n=== Threads with the most CPU time gained ===\n'
# Strip the parenthesized comm before splitting stat fields. A task name can
# contain spaces or closing parentheses. After stripping, field 1 is state.
awk -F '\t' '
    {
        stat = $2
        name = stat
        sub(/^[0-9]+ \(/, "", name)
        sub(/\) [^)]*$/, "", name)
        sub(/^.*\) /, "", stat)
        split(stat, field, " ")
        count = split($1, path, "/")
        key = path[count] ":" field[20]
    }
    FILENAME == ARGV[1] { user[key] = field[12]; kernel[key] = field[13]; next }
    (key in user) && !seen[key]++ {
        du = field[12] - user[key]
        dk = field[13] - kernel[key]
        if (du > 0 || dk > 0 || name == "Xorg" || name == "glxgears")
            printf "%d\t%s\t%s state=%s user_delta=%d system_delta=%d\n",
                du + dk, $1, name, field[1], du, dk
    }
' "$capture/before" "$capture/after" |
    busybox sort -k1,1nr > "$capture/ranked"
awk -F '\t' 'NR <= 8 || $3 ~ /^(Xorg|glxgears) /' "$capture/ranked" |
    while IFS="$(printf '\t')" read -r total thread details; do
        printf '%s %s total_ticks=%s\n' "$thread" "$details" "$total"
        cat "$thread/syscall" 2>/dev/null || true
    done

printf '\n=== Recent graphics kernel messages ===\n'
{
    if [ -r /proc/klog ]; then
        cat /proc/klog
    else
        dmesg
    fi
} 2>&1 | grep -Ei 'amdgpu|drm|vblank|flip|ring|fence|timeout|fault|denied' | tail -n 45 || true

awk '
    /^=== Threads with/ { summary = 1; print; next }
    /^=== Recent graphics/ { exit }
    summary && /^\/proc\// {
        selected = ++count <= 3 || $2 == "Xorg" || $2 == "glxgears"
    }
    summary && selected { print }
' "$report" >&3
printf 'Finished. Full report: %s\n' "$report" >&3
