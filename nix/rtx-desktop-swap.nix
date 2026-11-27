# RTX desktop profile update by package swap: copy an applied profile and repoint its symlinks
# from old store paths to rebuilt ones (for example brook-settings with the 5 % scaling UI).
# Only symlinks are rewritten; the build fails if a regular file names an old path or if a
# repointed target is missing from the new package.
#
#   nix-build nix/rtx-desktop-swap.nix --arg base '"/nix/store/...-brook-rtx-desktop-steam7"' \
#     --argstr name brook-rtx-desktop-seq8 \
#     --arg swaps '[ [ "/nix/store/<old>-brook-settings-0.1" "/nix/store/<new>-brook-settings-0.1" ] ]'
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; }, base, name, swaps }:
let
  baseProfile = builtins.storePath base;
  pairs = map (p: { old = builtins.elemAt p 0; new = builtins.storePath (builtins.elemAt p 1); }) swaps;
  rewrite = pkgs.lib.concatMapStringsSep "\n" (p: ''
    find "$out" -type l -lname '${p.old}*' | while IFS= read -r link; do
      target=$(readlink "$link"); repl="${p.new}''${target#${p.old}}"
      test -e "$repl" || { echo "missing $repl for $link" >&2; exit 1; }
      ln -sfn "$repl" "$link"
    done
    ! grep -rlF '${p.old}' "$out" 2>/dev/null | grep -q .
    ! find "$out" -type l -lname '${p.old}*' | grep -q .
  '') pairs;
in
pkgs.runCommand name { } ''
  mkdir -p "$out"
  cp -a ${baseProfile}/. "$out/"
  chmod -R u+w "$out"
  ${rewrite}
''
