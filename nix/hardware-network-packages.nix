# Networked Dell desktop candidate. Keep the small accepted probe reproducible.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; } }:
let
  base = import ./hardware-packages.nix { inherit pkgs; };
  fonts = import ../tools/brook-fonts-pkg {};
  cursors = import ../tools/brook-cursor-theme {};
  graphics = import ./runtime-graphics.nix { inherit pkgs; };
  firefox = import ../tools/firefox-brook-pkg { inherit pkgs; };
  chromium = import ../tools/chromium-brook-pkg { inherit pkgs; };
  gallery = import ../tools/brook-gallery-pkg { inherit pkgs; };
  launchers = pkgs.runCommand "brook-hardware-network-launchers" {} ''
    mkdir -p $out/bin $out/share/applications
    cp ${../data/nix-wrappers/brook-firefox} $out/bin/brook-firefox
    cp ${../data/nix-wrappers/brook-chromium} $out/bin/brook-chromium
    cat > $out/bin/brook-run <<'SCRIPT'
    #!${pkgs.bash}/bin/bash
    export PATH=/nix/bin:/nix/profile/bin:/boot/BIN:/data/bin
    export HOME=/data/home XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0
    export GDK_BACKEND=wayland SDL_VIDEODRIVER=wayland
    export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/brook-session-bus
    export FONTCONFIG_FILE=${fonts}/etc/fonts/fonts.conf FONTCONFIG_PATH=${fonts}/etc/fonts
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
    SCRIPT
    cat > $out/bin/hardware-brook-gallery <<'SCRIPT'
    #!${pkgs.bash}/bin/bash
    exec /nix/profile/bin/brook-run /nix/profile/bin/brook-gallery "$@"
    SCRIPT
    cat > $out/share/applications/brook-hardware-firefox.desktop <<'DESKTOP'
    [Desktop Entry]
    Type=Application
    Name=Firefox
    Exec=/nix/profile/bin/brook-firefox %U
    Icon=firefox
    Categories=Network;WebBrowser;
    DESKTOP
    cat > $out/share/applications/brook-hardware-chromium.desktop <<'DESKTOP'
    [Desktop Entry]
    Type=Application
    Name=Chromium
    Exec=/nix/profile/bin/brook-chromium %U
    Icon=chromium
    Categories=Network;WebBrowser;
    DESKTOP
    cat > $out/share/applications/brook-hardware-gallery.desktop <<'DESKTOP'
    [Desktop Entry]
    Type=Application
    Name=App Gallery
    Exec=/nix/profile/bin/hardware-brook-gallery
    Icon=brook-gallery
    Categories=System;PackageManager;
    DESKTOP
    chmod +x $out/bin/*
  '';
  packages = [ (pkgs.lib.hiPrio launchers) firefox chromium gallery graphics.mesaRuntime pkgs.xz.bin pkgs.zstd.bin ] ++ base.packages;
in {
  inherit packages firefox chromium gallery;
  profile = pkgs.buildEnv { name = "brook-network-hardware-desktop"; paths = packages; ignoreCollisions = true; };
}
