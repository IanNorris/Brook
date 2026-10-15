#!/nix/profile/bin/bash
# Stock SuperTuxKart, with per-user state on the writable ext2 data disk.
# Run as uid 1000 from the desktop; root gets a separate state directory.
set -e
export PATH=/nix/profile/bin:/nix/bin:/boot/BIN
export HOME=/data/home/supertuxkart-$UID
export XDG_CACHE_HOME=$HOME/.cache
export XDG_CONFIG_HOME=$HOME/.config
export XDG_DATA_HOME=$HOME/.local/share
mkdir -p "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
if [ ! -w "$XDG_CACHE_HOME" ] || [ ! -w "$XDG_CONFIG_HOME" ]; then
    printf 'SuperTuxKart needs writable per-user storage at %s\n' "$HOME" >&2
    exit 1
fi
export XDG_RUNTIME_DIR=/tmp
export WAYLAND_DISPLAY=wayland-0
export SDL_VIDEODRIVER=wayland
export SDL_AUDIODRIVER=dummy
export SDL_AUDIO_DRIVER=dummy
export ALSOFT_DRIVERS=null
exec /nix/profile/bin/supertuxkart --windowed --render-driver=gl "$@"
