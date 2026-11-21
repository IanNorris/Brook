{ pkgs, selectedProfiles ? false }:
let
  python = pkgs.python3.withPackages (ps: [ ps.pillow ps.cairosvg ]);
  # Keep the original launcher and support derivation byte-identical.
  support = pkgs.runCommand "brook-profile-stage-support" {} (''
    install -Dm644 ${../tools/nix-install/stage_profile.py} "$out/scripts/stage_profile.py"
    install -Dm644 ${../tools/nix-install/stage_launchers.py} "$out/scripts/stage_launchers.py"
    install -Dm644 ${../tools/nix-install/stage_nar.py} "$out/scripts/stage_nar.py"
    install -Dm644 ${../scripts/collect_desktop_entries.py} "$out/scripts/collect_desktop_entries.py"
    mkdir -p "$out/data/branding"
    cp -r ${../data/icons} "$out/data/icons"
    cp ${../data/branding/brook-mark.png} "$out/data/branding/brook-mark.png"
  '' + pkgs.lib.optionalString selectedProfiles ''
    install -Dm644 ${../tools/nix-install/stage_selected_launchers.py} "$out/scripts/stage_selected_launchers.py"
    ${pkgs.patch}/bin/patch --batch --fuzz=0 -d "$out/scripts" -p1 < ${../tools/nix-install/stage_selected_profile.patch}
  '');
in pkgs.writeShellScriptBin "brook-stage-profile" ''
  export BROOK_STAGE_OPENSSL=${pkgs.lib.getBin pkgs.openssl}/bin/openssl
  exec ${python}/bin/python3 -B -E -s ${support}/scripts/stage_profile.py "$@"
''
