{ pkgs, fonts, cursors, selectedProfile ? false }:
''
  #!${pkgs.bash}/bin/bash
  export PATH=/nix/bin:/nix/profile/bin:/boot/BIN:/data/bin
  export HOME=/data/home
  source ${../data/nix-wrappers/brook-session-env.sh} /tmp || exit 1
  export GDK_BACKEND=wayland,x11 SDL_VIDEODRIVER=wayland
  export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/brook-session-bus
  export FONTCONFIG_FILE=${if selectedProfile then "/nix/profile/etc/fonts/brook-selected.conf" else "${fonts}/etc/fonts/fonts.conf"} FONTCONFIG_PATH=${if selectedProfile then "/nix/profile/etc/fonts" else "${fonts}/etc/fonts"}
  export XCURSOR_PATH=${cursors}/share/icons XCURSOR_THEME=default
  export XDG_DATA_DIRS=/nix/profile/share
  export XDG_CONFIG_HOME=$HOME/.config XDG_CACHE_HOME=$HOME/.cache XDG_DATA_HOME=$HOME/.local/share
  export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt
  export LIBGL_ALWAYS_SOFTWARE=1
  export LIBGL_DRIVERS_PATH=/nix/profile/lib/dri
  export GBM_BACKENDS_PATH=/nix/profile/lib/gbm
  export __EGL_VENDOR_LIBRARY_DIRS=/nix/profile/share/glvnd/egl_vendor.d
  unset MESA_LOADER_DRIVER_OVERRIDE
  mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
  exec "$@"
''
