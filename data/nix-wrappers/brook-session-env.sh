# Sourced by brook-run after PATH is set. The argument is Brook's /tmp.
export XDG_RUNTIME_DIR="$1/brook-$EUID"
if ! mkdir -p -m 700 -- "$XDG_RUNTIME_DIR" ||
   ! [[ -d "$XDG_RUNTIME_DIR" && ! -L "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" ]] ||
   ! chmod 00700 -- "$XDG_RUNTIME_DIR"; then
    printf 'brook-run: cannot prepare private runtime directory %s\n' "$XDG_RUNTIME_DIR" >&2
    return 1
fi

# The compositor still listens on a shared socket. libwayland accepts the
# absolute path even when clients have separate runtime directories.
export XDG_SESSION_TYPE=wayland WAYLAND_DISPLAY="$1/wayland-0"

export XDG_CONFIG_DIRS="${XDG_CONFIG_DIRS:-/nix/profile/etc/xdg:/etc/xdg}"
export GSETTINGS_SCHEMA_DIR="${GSETTINGS_SCHEMA_DIR:-/nix/profile/share/brook/gsettings}"

# Qt's built-in KDE platform theme reads the user's kdeglobals and defaults
# to Breeze. Select it without setting a desktop identity for other toolkits.
if [[ -z "${QT_QPA_PLATFORMTHEME:-}" ]]; then
    export QT_QPA_PLATFORMTHEME=kde KDE_SESSION_VERSION="${KDE_SESSION_VERSION:-6}"
fi
