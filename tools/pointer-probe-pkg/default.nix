{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-pointer-probe";
  version = "1";
  src = ./.;
  buildInputs = [ pkgs.SDL2 ];
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildPhase = ''
    $CC -O2 -Wall -Wextra -Werror pointer-probe.c $(pkg-config --cflags --libs sdl2) -o brook-pointer-probe
  '';
  installPhase = ''
    mkdir -p $out/bin
    cp brook-pointer-probe $out/bin/
  '';
}
