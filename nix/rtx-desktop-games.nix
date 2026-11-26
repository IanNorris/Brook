# RTX desktop games overlay: Steam, Quake II and Quake III on CPU rendering.
#
# The RTX desktop (usb-system mode) has no GPU 3D yet, so every GL path here
# is Mesa llvmpipe. The overlay copies an existing selected desktop profile
# and adds launchers, so a signed profile plan built from it only fetches the
# new store paths; nothing already on the SSD changes.
#
#   nix-build nix/rtx-desktop-games.nix --arg base '"/nix/store/...-brook-selected-desktop"' \
#     --arg quake2Data '<dir with baseq2/>' --arg quake3Data '<dir with baseq3/>'
#
# Game data is user-supplied (retail Quake II paks; the Quake III demo pak
# plus the public 1.32 point-release paks). It is never part of the repo.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; }
, base
, quake2Data ? null
, quake3Data ? null
}:
let
  inherit (pkgs) lib;
  runtime = import ./runtime-graphics.nix { inherit pkgs; };
  mesa = runtime.mesaRuntime;
  steamPkgs = import ./nixpkgs.nix {
    config.allowUnfreePredicate = package: lib.getName package == "brook-steam-client";
  };
  steam = import ../tools/brook-steam-pkg { inherit pkgs steamPkgs; };
  yquake = import ../tools/yquake2-brook-pkg { inherit pkgs; };
  openal = import ../tools/openal-soft-oss-pkg { inherit pkgs; };
  q3 = pkgs.ioquake3;
  baseProfile = builtins.storePath base;
  dataDir = name: path: if path == null then null else builtins.path { inherit name path; };
  q2data = dataDir "brook-quake2-data" quake2Data;
  q3data = dataDir "brook-quake3-data" quake3Data;
  alsoftConf = pkgs.writeText "brook-quake2-alsoft-oss.conf" (builtins.readFile ../data/config/alsoft-oss.conf);

  # llvmpipe for EGL (SDL Wayland) and GLX (Xwayland) through glvnd.
  softGl = ''
    export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
    export LIBGL_DRIVERS_PATH=${mesa}/lib/dri GBM_BACKENDS_PATH=${mesa}/lib/gbm
    unset __EGL_VENDOR_LIBRARY_DIRS MESA_LOADER_DRIVER_OVERRIDE
    export __EGL_VENDOR_LIBRARY_FILENAMES=${mesa}/share/glvnd/egl_vendor.d/50_mesa.json
    export __GLX_VENDOR_LIBRARY_NAME=mesa
    export LD_LIBRARY_PATH=${mesa}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
  '';
  script = name: body: pkgs.writeTextFile {
    inherit name;
    executable = true;
    destination = "/bin/${name}";
    text = "#!${pkgs.bash}/bin/bash\nset -u\n" + body;
    checkPhase = "${pkgs.bash}/bin/bash -n $out/bin/${name}";
  };

  # Quake II: yquake2 GL3 (override with BROOK_QUAKE2_RENDERER=gl1|soft), OSS OpenAL.
  quake2 = script "brook-quake2-soft" ''
    if [ "$EUID" -eq 0 ]; then echo 'brook-quake2-soft: run as uid 1000' >&2; exit 64; fi
    DATA=''${BROOK_QUAKE2_DATA_DIR:-${toString q2data}}
    if [ "''${1:-}" = --check ]; then
      [ -x ${yquake}/lib/yquake2/quake2 ] && [ -f "$DATA/baseq2/pak0.pak" ] && \
        [ -e ${openal}/lib/libopenal.so.1 ] && [ -e ${mesa}/lib/libGLX_mesa.so.0 ] && \
        { echo BROOK_QUAKE2_CHECK_OK; exit 0; }
      echo BROOK_QUAKE2_CHECK_MISSING; exit 1
    fi
    ${softGl}
    export LD_LIBRARY_PATH=${openal}/lib:$LD_LIBRARY_PATH ALSOFT_CONF=${alsoftConf}
    exec /nix/profile/bin/brook-run env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
      ${yquake}/lib/yquake2/quake2 -datadir "$DATA" \
      +set vid_renderer "''${BROOK_QUAKE2_RENDERER:-gl3}" \
      +set s_openal 1 +set al_driver libopenal.so.1 "$@"
  '';

  # Quake III: ioquake3 with the demo data and native ui/cgame/game modules.
  quake3 = script "brook-quake3-soft" ''
    if [ "$EUID" -eq 0 ]; then echo 'brook-quake3-soft: run as uid 1000' >&2; exit 64; fi
    DATA=''${BROOK_QUAKE3_DATA_DIR:-${toString q3data}}
    if [ "''${1:-}" = --check ]; then
      [ -x ${q3}/share/ioquake3/ioquake3.x86_64 ] && [ -f "$DATA/baseq3/pak0.pk3" ] && \
        [ -e ${mesa}/lib/libGLX_mesa.so.0 ] && { echo BROOK_QUAKE3_CHECK_OK; exit 0; }
      echo BROOK_QUAKE3_CHECK_MISSING; exit 1
    fi
    ${softGl}
    HOMEPATH=/data/home/.local/share/brook/quake3
    mkdir -p "$HOMEPATH"
    exec /nix/profile/bin/brook-run env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" SDL_AUDIODRIVER=alsa \
      ${q3}/share/ioquake3/ioquake3.x86_64 \
      +set fs_basepath "$DATA" +set fs_homepath "$HOMEPATH" \
      +set fs_libpath ${q3}/share/ioquake3 \
      +set vm_ui 0 +set vm_game 0 +set vm_cgame 0 +set s_useOpenAL 0 \
      +set cl_renderer "''${BROOK_QUAKE3_RENDERER:-opengl1}" +set cg_drawFPS 1 "$@"
  '';

  # One-shot timedemos; each prints a single JSON line with the engine's result.
  bench = script "brook-games-bench" ''
    # usage: brook-games-bench q2 [gl3|gl1|soft] | q3 [opengl1|opengl2]
    game=''${1:-q2}; renderer=''${2:-}; limit=''${BROOK_BENCH_TIMEOUT:-240}
    out=/data/home/.local/share/brook/bench; mkdir -p "$out"
    log=$out/$game-''${renderer:-default}-$(${pkgs.coreutils}/bin/date +%s).log
    case $game in
      q2) BROOK_QUAKE2_RENDERER=''${renderer:-gl3} ${pkgs.coreutils}/bin/timeout "$limit" \
            ${quake2}/bin/brook-quake2-soft +set vid_fullscreen 0 +set r_mode 4 \
            +set vid_maxfps 1000 +set r_vsync 0 +set timedemo 1 +demomap demo1.dm2 >"$log" 2>&1 &
          pattern='frames, .* seconds: .* fps' ;;
      q3) BROOK_QUAKE3_RENDERER=''${renderer:-opengl1} ${pkgs.coreutils}/bin/timeout "$limit" \
            ${quake3}/bin/brook-quake3-soft +set r_fullscreen 0 +set r_mode 4 \
            +set r_swapInterval 0 +set com_maxfps 0 +set timedemo 1 +demo four >"$log" 2>&1 &
          pattern='frames .* seconds .* fps' ;;
      *) echo 'usage: brook-games-bench q2 [gl3|gl1|soft] | q3 [opengl1|opengl2]' >&2; exit 64 ;;
    esac
    pid=$!
    for _ in $(${pkgs.coreutils}/bin/seq "$limit"); do
      result=$(${pkgs.gnugrep}/bin/grep -a -m1 -E "$pattern" "$log" || :)
      [ -n "$result" ] && break
      kill -0 "$pid" 2>/dev/null || break
      ${pkgs.coreutils}/bin/sleep 1
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    renderer_line=$(${pkgs.gnugrep}/bin/grep -a -m1 -i -E 'GL_RENDERER|renderer:' "$log" | ${pkgs.coreutils}/bin/tr -d '"\\' || :)
    printf '{"game":"%s","renderer":"%s","result":"%s","gl":"%s","log":"%s"}\n' \
      "$game" "''${renderer:-default}" "$result" "$renderer_line" "$log"
    [ -n "$result" ]
  '';

  # Steam: the pinned x86-64 client on the rootless desktop Xwayland, with
  # llvmpipe GLX from the Xwayland wrapper below. No nested Xephyr.
  steamDesktop = script "brook-steam-soft" ''
    ${softGl}
    export BROOK_STEAM_FULL_PYTHON=${pkgs.python3}/bin/python3
    exec ${steam.launcher}/bin/brook-steam --gl software "$@"
  '';

  # waylandd starts /nix/profile/bin/Xwayland with "-glamor gl". With no GPU
  # that server has no GLX, which Steam needs. Start it the way waylandd's
  # BROOK_XWAYLAND_SOFTWARE_GLX=1 mode does (-shm +extension GLX +iglx,
  # llvmpipe for the server only); this was the wrapper that opened Steam on
  # the Dell's software desktop. exec keeps the pid and inherited fds.
  xwayland = script "Xwayland" ''
    args=()
    while [ $# -gt 0 ]; do
      if [ "$1" = -glamor ]; then shift 2; continue; fi
      args+=("$1"); shift
    done
    export LIBGL_DRIVERS_PATH=${mesa}/lib/dri LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
    exec ${baseProfile}/bin/Xwayland "''${args[@]}" -shm +extension GLX +iglx
  '';

  desktop = id: name: exec: icon: categories: ''
    cat > "$out/share/applications/${id}.desktop" <<'DESKTOP'
    [Desktop Entry]
    Type=Application
    Name=${name}
    Exec=/nix/profile/bin/${exec}
    Icon=${icon}
    Categories=${categories}
    DESKTOP
  '';
in
assert q2data != null && q3data != null || throw "rtx-desktop-games needs quake2Data and quake3Data";
pkgs.runCommand "brook-selected-desktop-games" {
  passthru = { inherit steam mesa yquake openal q3 q2data q3data; };
} ''
  # Materialise a symlinked directory so new entries can be added beside it.
  own() {
    if [ -L "$1" ]; then
      target=$(readlink -f "$1"); rm "$1"; mkdir "$1"
      for entry in "$target"/* "$target"/.[!.]*; do [ -e "$entry" ] && ln -s "$entry" "$1/" || :; done
    fi
    mkdir -p "$1"; chmod u+w "$1"
  }
  mkdir -p "$out"
  cp -a ${baseProfile}/. "$out"
  chmod u+w "$out" "$out/bin" "$out/lib" "$out/share" "$out/share/applications" "$out/share/icons"
  own "$out/share/icons/hicolor"
  for size in 256x256 scalable; do own "$out/share/icons/hicolor/$size"; own "$out/share/icons/hicolor/$size/apps"; done

  ln -s ${quake2}/bin/brook-quake2-soft ${quake3}/bin/brook-quake3-soft \
        ${bench}/bin/brook-games-bench ${steamDesktop}/bin/brook-steam-soft "$out/bin/"
  ln -s ${steam.launcher}/bin/brook-steam "$out/bin/brook-steam"
  rm "$out/bin/Xwayland"; ln -s ${xwayland}/bin/Xwayland "$out/bin/Xwayland"
  # Profile-wide llvmpipe for brook-run's defaults (LIBGL_DRIVERS_PATH, EGL vendor dir).
  ln -s ${mesa}/lib/dri "$out/lib/dri"
  ln -s ${mesa}/lib/gbm "$out/lib/gbm"
  ln -s ${mesa}/lib/libGLX_mesa.so.0 "$out/lib/libGLX_mesa.so.0"
  own "$out/share/glvnd"
  ln -s ${mesa}/share/glvnd/egl_vendor.d "$out/share/glvnd/egl_vendor.d"

  cp ${yquake.src}/stuff/icon/Quake2.png "$out/share/icons/hicolor/256x256/apps/brook-quake2.png"
  cp ${q3}/share/icons/hicolor/scalable/apps/ioquake3.svg "$out/share/icons/hicolor/scalable/apps/brook-quake3.svg"
  ${desktop "brook-quake2" "Quake II" "brook-quake2-soft" "brook-quake2" "Game;ActionGame;"}
  ${desktop "brook-quake3" "Quake III Arena (Demo)" "brook-quake3-soft" "brook-quake3" "Game;ActionGame;"}
  ${desktop "brook-hardware-steam" "Steam" "brook-steam-soft" "applications-games" "Game;"}
  for entry in "$out"/share/applications/*.desktop; do test -e "$entry"; done
''
