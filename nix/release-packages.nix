# Public candidate: explicit open-source payload, never an existing review disk.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; } }:
let
  fonts = import ../tools/brook-fonts-pkg {};
  cursors = import ../tools/brook-cursor-theme {};
  chromium = import ../tools/chromium-brook-pkg { inherit pkgs; downstreamStaging = true; };
  vlc = import ../tools/vlc-brook-pkg { inherit pkgs; };
  graphics = import ./runtime-graphics.nix { inherit pkgs; };
  native = map (name: let f = import (../tools + "/${name}"); in
    f (if builtins.hasAttr "pkgs" (builtins.functionArgs f) then { inherit pkgs; } else {})) [
    "waylandd-pkg" "wayland-calc-pkg" "brook-player-pkg" "brook-files-pkg"
    "brook-gallery-pkg" "brook-settings-pkg" "brook-message-pkg" "brook-session-pkg" "locale-brook-pkg"
  ];
  launchers = pkgs.runCommand "brook-release-launchers" { meta.license = pkgs.lib.licenses.mit; } ''
    mkdir -p $out/bin
    cp ${../data/nix-wrappers/brook-chromium} $out/bin/brook-chromium
    cat > $out/bin/brook-run <<'SCRIPT'
    #!${pkgs.bash}/bin/bash
    export PATH=/nix/bin:/nix/profile/bin:/boot/BIN:/data/bin
    export HOME=/data/home
    export XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0
    export GDK_BACKEND=wayland SDL_VIDEODRIVER=wayland
    export FONTCONFIG_FILE=${fonts}/etc/fonts/fonts.conf FONTCONFIG_PATH=${fonts}/etc/fonts
    export XCURSOR_PATH=${cursors}/share/icons XCURSOR_THEME=default
    export XDG_DATA_DIRS=/nix/profile/share
    export XDG_CONFIG_HOME=$HOME/.config XDG_CACHE_HOME=$HOME/.cache XDG_DATA_HOME=$HOME/.local/share
    export LIBGL_ALWAYS_SOFTWARE=''${LIBGL_ALWAYS_SOFTWARE:-1}
    export LIBGL_DRIVERS_PATH=/nix/profile/lib/dri
    export GBM_BACKENDS_PATH=/nix/profile/lib/gbm
    export __EGL_VENDOR_LIBRARY_DIRS=/nix/profile/share/glvnd/egl_vendor.d
    if [ "$LIBGL_ALWAYS_SOFTWARE" = 1 ]; then
      unset MESA_LOADER_DRIVER_OVERRIDE
      export VK_DRIVER_FILES=/nix/profile/share/vulkan/icd.d/lvp_icd.x86_64.json
    fi
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
    exec "$@"
    SCRIPT
    cat > $out/bin/brook-vlc <<'SCRIPT'
    #!${pkgs.bash}/bin/bash
    exec /nix/profile/bin/brook-run /nix/profile/bin/vlc --no-plugins-cache --avcodec-hw=none --avcodec-threads=1 --no-qt-privacy-ask --vout=wl_shm --aout=oss --oss-audio-device=/dev/dsp "$@"
    SCRIPT
    chmod +x $out/bin/*
  '';
  packages = [
    pkgs.bash pkgs.coreutils pkgs.curl.bin pkgs.xz.bin pkgs.zstd.bin pkgs.cacert
    pkgs.gnutar pkgs.unzip pkgs.python3 pkgs.gitMinimal pkgs.vim pkgs.tinycc
    pkgs.ffmpeg.bin pkgs.wayland graphics.mesaRuntime pkgs.tzdata
    pkgs.hicolor-icon-theme pkgs.adwaita-icon-theme pkgs.shared-mime-info
    pkgs.gimp pkgs.qalculate-gtk pkgs.xfce.mousepad pkgs.openttd pkgs.gnome-commander
    chromium vlc fonts cursors launchers
  ] ++ native;
in {
  inherit packages;
  profile = pkgs.buildEnv { name = "brook-public-desktop"; paths = packages; ignoreCollisions = true; };
}
