# Offline Dell bring-up payload: native Wayland/GTK apps and a real shell.
# Deliberately excludes browsers, games, Mesa/LLVM and account/private data.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; }, includeOfflineApps ? true }:
let
  files = import ./desktop-files.nix { inherit pkgs; };
  fonts = import ../tools/brook-fonts-pkg {};
  cursors = import ../tools/brook-cursor-theme {};
  nativePackage = name:
    let f = import (../tools + "/${name}"); in
    f (if builtins.hasAttr "pkgs" (builtins.functionArgs f) then { inherit pkgs; } else {});
  native = [ (nativePackage "waylandd-pkg") ]
    ++ pkgs.lib.optional includeOfflineApps (nativePackage "wayland-calc-pkg")
    ++ map nativePackage [
      "brook-settings-pkg" "brook-message-pkg" "brook-session-pkg"
      "brook-apps-pkg" "brook-task-manager-pkg"
    ];
  launchers = pkgs.runCommand "brook-hardware-launchers" {} ''
    mkdir -p $out/bin $out/share/applications
    cat > $out/bin/brook-run <<'SCRIPT'
    #!${pkgs.bash}/bin/bash
    export PATH=/nix/profile/bin:/boot/BIN
    export HOME=/data/home DISPLAY=:0
    source ${../data/nix-wrappers/brook-session-env.sh} /tmp || exit 1
    export GDK_BACKEND=wayland SDL_VIDEODRIVER=wayland
    export FONTCONFIG_FILE=${fonts}/etc/fonts/fonts.conf FONTCONFIG_PATH=${fonts}/etc/fonts
    export XCURSOR_PATH=${cursors}/share/icons XCURSOR_THEME=default
    export XDG_DATA_DIRS=/nix/profile/share
    export XDG_CONFIG_HOME=$HOME/.config XDG_CACHE_HOME=$HOME/.cache XDG_DATA_HOME=$HOME/.local/share
    export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
    exec "$@"
    SCRIPT
    for app in ${if includeOfflineApps then "mousepad brook-settings wayland-calc brook-task-manager" else "brook-settings brook-task-manager"}; do
      cat > $out/bin/hardware-$app <<SCRIPT
    #!${pkgs.bash}/bin/bash
    exec /nix/profile/bin/brook-run /nix/profile/bin/$app "\$@"
    SCRIPT
    done
    cat > $out/bin/hardware-dbus <<'SCRIPT'
    #!${pkgs.bash}/bin/bash
    export PATH=/nix/profile/bin:/boot/BIN
    export XDG_RUNTIME_DIR=/tmp/brook-1000
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
    exec /nix/profile/bin/brook-dbus --config-file=/nix/profile/bin/brook-session.conf --address=unix:path=/tmp/brook-session-bus --nofork
    SCRIPT
    cat > $out/bin/hardware-message <<'SCRIPT'
    #!${pkgs.bash}/bin/bash
    export PATH=/nix/profile/bin:/boot/BIN
    for attempt in {1..100}; do
      test -S /tmp/wayland-0 && break
      sleep 0.1
    done
    exec /nix/profile/bin/brook-run /nix/profile/bin/brook-message --watch
    SCRIPT
    chmod +x $out/bin/*
  '';
  packages = [
    pkgs.bashInteractive pkgs.coreutils pkgs.curl.bin pkgs.cacert pkgs.gnutar
    pkgs.unzip pkgs.vim
  ] ++ pkgs.lib.optional includeOfflineApps pkgs.xfce.mousepad ++ [
    pkgs.tzdata pkgs.xwayland
    # Steam's game-runtime entry point uses util-linux getopt before entering
    # its container; ship it in the profile rather than relying on /usr/bin.
    (pkgs.lib.getBin pkgs.util-linuxMinimal)
    pkgs.hicolor-icon-theme pkgs.adwaita-icon-theme pkgs.kdePackages.breeze-icons pkgs.shared-mime-info
    fonts cursors launchers
  ] ++ pkgs.lib.optionals includeOfflineApps [ files.integration files.gvfs ] ++ [
    (import ./desktop-settings.nix { inherit pkgs; inherit (files) nautilus; })
  ] ++ native;
in {
  inherit packages;
  profile = pkgs.buildEnv { name = "brook-offline-hardware-desktop"; paths = packages; ignoreCollisions = true; };
}
