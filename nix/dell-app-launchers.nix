# Dell Apps launchers staged into an existing desktop: Quake II and VSCodium.
#
# Build:   nix-build nix/dell-app-launchers.nix --no-out-link
# Stage:   tools/launcher-stage/make_bundle.py (see tools/launcher-stage
#          "Quake II and VSCodium Apps entries").
#
# The store closure is deliberately small (bash and the OpenAL config): the
# game, OpenAL and Iris paths are recorded as plain strings, not Nix
# references. On the Dell the complete copies of yquake2 and the OSS OpenAL
# live in private stage directories, and the top-level OpenAL path is known to
# be incomplete (no lib/), so a closure reference would either be missing or
# point at a broken path. brook-quake2 checks each candidate at run time
# (`brook-quake2 --check` reports the choice) and prefers the top-level path of
# the same repo derivation once it is complete.
{ pkgs ? import ./nixpkgs.nix {} }:
let
  inherit (builtins) unsafeDiscardStringContext baseNameOf;
  yquake = import ../tools/yquake2-brook-pkg { inherit pkgs; };
  openal = import ../tools/openal-soft-oss-pkg { inherit pkgs; };
  iris = import ../tools/mesa-iris-brook-pkg/runtime.nix { inherit pkgs; };
  plain = drv: unsafeDiscardStringContext "${drv}";
  yquakePath = plain yquake;
  openalPath = plain openal;
  alsoftConf = pkgs.writeText "brook-quake2-alsoft-oss.conf" (builtins.readFile ../data/config/alsoft-oss.conf);
  # 24x24 straight-alpha little-endian ARGB (bytes B,G,R,A), the launcher's icon format.
  quakeIcon = pkgs.runCommand "brook-quake2-icon" { nativeBuildInputs = [ pkgs.imagemagick ]; } ''
    magick ${yquake.src}/stuff/icon/Quake2.png -background none -resize 24x24 -gravity center -extent 24x24 -depth 8 BGRA:$out
    test "$(stat -c %s $out)" = 2304
  '';
in pkgs.runCommand "brook-dell-app-launchers" {} ''
  mkdir -p $out/bin $out/share/applications $out/share/brook-icons $out/share/brook-quake2
  cp ${alsoftConf} $out/share/brook-quake2/alsoft-oss.conf
  cp ${quakeIcon} $out/share/brook-icons/brook-quake2.rgba

  substitute ${../data/nix-wrappers/brook-quake2.in} $out/bin/brook-quake2 \
    --subst-var-by BASH ${pkgs.bash}/bin/bash \
    --subst-var-by YQUAKE2_NIX ${yquakePath} \
    --subst-var-by YQUAKE2_STAGE /nix/store/.brook-gpu-stage-c6f05db4/${baseNameOf yquakePath} \
    --subst-var-by OPENAL_NIX ${openalPath} \
    --subst-var-by OPENAL_STAGE /nix/store/.brook-audio-stage/${baseNameOf openalPath} \
    --subst-var-by IRIS_VERIFIED /nix/store/998k7n885vyvxlsckcc5nf9avj3d540n-mesa-26.0.6 \
    --subst-var-by IRIS_NIX ${plain iris} \
    --subst-var-by ALSOFT_CONF $out/share/brook-quake2/alsoft-oss.conf

  # Electron's SUID sandbox is not set up on Brook; the wrapper passes --no-sandbox.
  { echo '#!${pkgs.bash}/bin/bash'; tail -n +2 ${../data/nix-wrappers/brook-codium}; } > $out/bin/brook-codium

  cat > $out/share/applications/brook-quake2.desktop <<'DESKTOP'
  [Desktop Entry]
  Type=Application
  Name=Quake II
  Exec=/nix/profile/bin/brook-quake2
  Icon=brook-quake2
  Categories=Game;ActionGame;
  DESKTOP
  # Apps-menu edits for tools/launcher-stage (format in install.py): add Quake
  # II, and point every existing entry that runs codium at the wrapper.
  mkdir -p $out/share/brook-stage
  cat > $out/share/brook-stage/apps.json <<JSON
  {
   "icons": {"brook-quake2.rgba": "$out/share/brook-icons/brook-quake2.rgba"},
   "upsert": [{"app_id": "brook-quake2", "name": "Quake II",
               "exec": "--uid 1000 /nix/profile/bin/brook-quake2",
               "icon": "brook-quake2.rgba", "categories": "Game;ActionGame;"}],
   "rewrite_exec": [{"program": "codium", "exec": "--uid 1000 /nix/profile/bin/brook-codium"},
                    {"program": "vscodium", "exec": "--uid 1000 /nix/profile/bin/brook-codium"}]
  }
  JSON
  chmod 0755 $out/bin/*
  ${pkgs.bash}/bin/bash -n $out/bin/brook-quake2
  ${pkgs.bash}/bin/bash -n $out/bin/brook-codium
''
