# One package pin and a recorded closure for the experiment disks.
{ pkgs ? import ./nixpkgs.nix {} }:
let
  waylandd = import ../tools/waylandd-pkg {};
  calc = import ../tools/wayland-calc-pkg {};
  player = import ../tools/brook-player-pkg {};
  files = import ../tools/brook-files-pkg {};
  fonts = import ../tools/brook-fonts-pkg {};
  cursors = import ../tools/brook-cursor-theme {};
  yquake = import ../tools/yquake2-brook-pkg { inherit pkgs; };
  yquakeSoft = import ../tools/yquake2-pkg { inherit pkgs; };
  openal = import ../tools/openal-soft-oss-pkg { inherit pkgs; };
  quakeWrapper = package: pkgs.replaceVars ../data/nix-wrappers/yquake2-play.sh.in {
    BASH_BIN = "${pkgs.bash}/bin/bash";
    MESA_PATH = "${pkgs.mesa}";
    YQUAKE2_BIN = "${package}/lib/yquake2/quake2";
    YQUAKE2_LIBDIRS = "${openal}/lib:${package}/lib/yquake2";
  };
  launchers = pkgs.runCommand "brook-experiment-launchers" {} ''
    mkdir -p $out/bin
    cp ${quakeWrapper yquake} $out/bin/brook-yquake2-gl
    cp ${quakeWrapper yquakeSoft} $out/bin/brook-yquake2-soft
    substituteInPlace $out/bin/brook-yquake2-soft --replace-fail 'RENDERER="gl1"' 'RENDERER="soft"'
    cat > $out/bin/brook-run <<'EOF'
    #!${pkgs.bash}/bin/bash
    export PATH=/nix/profile/bin:/boot/BIN:/data/bin
    export XDG_RUNTIME_DIR=/tmp
    export WAYLAND_DISPLAY=wayland-0
    export GDK_BACKEND=wayland
    export SDL_VIDEODRIVER=wayland
    export GTK_THEME=Adwaita
    export FONTCONFIG_FILE=${fonts}/etc/fonts/fonts.conf
    export FONTCONFIG_PATH=${fonts}/etc/fonts
    export XCURSOR_PATH=${cursors}/share/icons
    export XCURSOR_THEME=default
    export XDG_DATA_DIRS=/nix/profile/share
    exec "$@"
    EOF
    cat > $out/bin/brook-ladybird <<'EOF'
    #!${pkgs.bash}/bin/bash
    export HOME=/data/home
    export XDG_CACHE_HOME=$HOME/.cache
    export XDG_CONFIG_HOME=$HOME/.config
    export XDG_DATA_HOME=$HOME/.local/share
    /nix/profile/bin/mkdir -p "$XDG_CACHE_HOME/Ladybird" "$XDG_CONFIG_HOME/Ladybird" "$XDG_DATA_HOME/Ladybird"
    exec /nix/profile/bin/brook-run /nix/profile/bin/Ladybird \
      --force-cpu-painting --disable-sql-database --disable-http-disk-cache "$@"
    EOF
    cat > $out/bin/brook-office <<'EOF'
    #!${pkgs.bash}/bin/bash
    export HOME=/data/home
    exec /nix/profile/bin/brook-run ${pkgs.libreoffice}/bin/libreoffice \
      -env:UserInstallation=file:///data/home/libreoffice \
      --nofirststartwizard --norestore --writer "$@"
    EOF
    cat > $out/bin/brook-vlc <<'EOF'
    #!${pkgs.bash}/bin/bash
    export HOME=/data/home
    export XDG_CACHE_HOME=$HOME/.cache
    export XDG_CONFIG_HOME=$HOME/.config
    export XDG_DATA_HOME=$HOME/.local/share
    /nix/profile/bin/mkdir -p "$XDG_CONFIG_HOME/vlc"
    exec /nix/profile/bin/brook-run /nix/profile/bin/vlc \
      --no-plugins-cache --avcodec-hw=none --avcodec-threads=1 \
      --no-qt-privacy-ask --vout=wl_shm --aout=oss --oss-audio-device=/dev/dsp "$@"
    EOF
    chmod +x $out/bin/*
  '';
in {
  inherit yquake yquakeSoft openal player waylandd calc;
  profile = pkgs.buildEnv {
    name = "brook-experiment-profile";
    paths = [
      pkgs.bash pkgs.coreutils pkgs.curl.bin pkgs.xz.bin pkgs.zstd.bin pkgs.cacert
      pkgs.python3 pkgs.ffmpeg.bin pkgs.wayland pkgs.mesa
      pkgs.libreoffice pkgs.gimp pkgs.qalculate-gtk pkgs.xfce.mousepad
      waylandd calc player files fonts cursors launchers
    ];
    ignoreCollisions = true;
  };
}
