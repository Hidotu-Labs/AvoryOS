#!/bin/sh
# AvoryOS VLC launcher; also used by qvlc/cvlc and the desktop entry.
export VLC_PLUGIN_PATH="/usr/lib/vlc/plugins${VLC_PLUGIN_PATH:+:$VLC_PLUGIN_PATH}"
: "${DISPLAY:=:0}"
export DISPLAY
exec /usr/bin/vlc.bin -I qt "$@"
