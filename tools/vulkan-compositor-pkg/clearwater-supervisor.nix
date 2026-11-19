{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.pkgsCross.musl64.stdenv.mkDerivation {
  pname = "brook-clearwater-supervisor";
  version = "0.1";
  dontUnpack = true;
  nativeBuildInputs = [ pkgs.python3 ];
  buildPhase = ''
    $CC -std=c11 -O2 -Wall -Wextra -Werror -static \
      ${./clearwater_supervisor.c} -o CWSUP.ELF
    python3 ${./clearwater_supervisor_test.py} ./CWSUP.ELF
  '';
  installPhase = ''
    install -Dm755 CWSUP.ELF "$out/bin/CWSUP.ELF"
  '';
}
