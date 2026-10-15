# Export one stock application from the same pin as Brook's SDK.
{ pkgs ? import ../../nix/nixpkgs.nix {}, app ? "vim" }:
let
  applications = {
    inherit (pkgs) vim helix emacs rustc vscodium blender firefox chromium;
  };
in if builtins.hasAttr app applications then applications.${app}
   else throw "Unknown Brook workload application: ${app}"
