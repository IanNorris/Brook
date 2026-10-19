{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-signal-context-probe";
  version = "0.1";
  src = ./.;
  dontConfigure = true;
  buildPhase = ''
    $CC -std=gnu11 -O2 -Wall -Wextra -Werror -fno-omit-frame-pointer probe.c -o signal-context-probe
  '';
  doCheck = true;
  checkPhase = "./signal-context-probe";
  installPhase = "mkdir -p $out/bin; cp signal-context-probe $out/bin/";
}
