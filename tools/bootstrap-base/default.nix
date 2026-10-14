{ pkgs ? import ../../nix/nixpkgs.nix {}, withSdk ? false }:
pkgs.buildEnv {
  name = if withSdk then "brook-bootstrap-prepared" else "brook-bootstrap-base";
  paths = [ pkgs.bash pkgs.coreutils pkgs.curl pkgs.xz pkgs.zstd pkgs.cacert ]
    ++ pkgs.lib.optional withSdk (import ../bootstrap-pkg { inherit pkgs; });
  ignoreCollisions = true;
}
