# RTX desktop profile seq6: the seq5 profile (NVK tools) with a brook-steam launcher that runs
# games through the native64 reaper (Steam's steamrt64/reaper is i386, so every native game
# exited 126) and prepares a shared /tmp/dumps. The new launcher is derived from the profile's
# existing one: the same seed, provider, runtime and python store paths (already on the SSD),
# with only brook-steam.py replaced and BROOK_STEAM_REAPER added. New store paths: the
# launcher, the reaper and the profile.
#
#   nix-build nix/rtx-desktop-steam6.nix --arg base '"/nix/store/...-brook-rtx-desktop-nvk"' \
#     --arg oldLauncher '"/nix/store/...-brook-steam-launcher-<version>"'
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; }, base, oldLauncher }:
let
  baseProfile = builtins.storePath base;
  previous = builtins.storePath oldLauncher;
  reaper = import ../tools/reaper-pkg { inherit pkgs; };
  launcher = pkgs.runCommand "brook-steam-launcher-reaper" {} ''
    mkdir -p "$out/bin" "$out/libexec"
    cp -a ${previous}/libexec/. "$out/libexec/"
    chmod u+w "$out/libexec" "$out/libexec/brook-steam.py"
    install -m644 ${../scripts/workloads/brook_steam_launcher.py} "$out/libexec/brook-steam.py"
    grep -q 'libexec/brook-steam.py' ${previous}/bin/brook-steam
    sed -e "s|${previous}|$out|g" \
        -e "s|^exec |export BROOK_STEAM_REAPER=${reaper}/bin/reaper\nexec |" \
        ${previous}/bin/brook-steam > "$out/bin/brook-steam"
    chmod 755 "$out/bin/brook-steam"
    grep -q "BROOK_STEAM_REAPER=${reaper}/bin/reaper" "$out/bin/brook-steam"
    ! grep -q "${previous}" "$out/bin/brook-steam"
  '';
in
pkgs.runCommand "brook-rtx-desktop-steam6" { passthru = { inherit launcher reaper; }; } ''
  mkdir -p "$out"
  cp -a ${baseProfile}/. "$out/"
  chmod u+w "$out" "$out/bin"
  test "$(readlink "$out/bin/brook-steam")" = ${previous}/bin/brook-steam
  rm "$out/bin/brook-steam"
  ln -s ${launcher}/bin/brook-steam "$out/bin/brook-steam"
  # brook-steam-soft (the .desktop entry's command) names the launcher by store path.
  soft=$(readlink -f "$out/bin/brook-steam-soft")
  grep -q ${previous} "$soft"
  rm "$out/bin/brook-steam-soft"
  sed "s|${previous}|${launcher}|g" "$soft" > "$out/bin/brook-steam-soft"
  chmod 755 "$out/bin/brook-steam-soft"
  ! grep -rl ${previous} "$out/bin" 2>/dev/null | grep -q .
''
