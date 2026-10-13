# Sourced by Xorg/XFCE xinitrc; run in a child so failures cannot exit it.
# SDDM configures outputs before the greeter; startxfce4 needs this hook.
if [ -x /usr/bin/avory-prime-display ]; then
    /usr/bin/avory-prime-display ||
        printf '%s\n' '[PRIME] Failed to connect or enable display outputs' >&2
fi
