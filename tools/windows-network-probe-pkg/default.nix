{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.pkgsCross.mingwW64.stdenv.mkDerivation {
  pname = "brook-windows-network-probe";
  version = "0.1";
  src = ./.;
  buildPhase = ''
    $CC -std=c11 -O2 -Wall -Wextra -Werror -static -D__USE_MINGW_ANSI_STDIO=0 \
      probe.c -liphlpapi -lws2_32 -o windows-network-probe.exe
  '';
  installPhase = ''
    mkdir -p $out/bin
    cp windows-network-probe.exe $out/bin/
  '';
}
