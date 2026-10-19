{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.pkgsCross.mingwW64.stdenv.mkDerivation {
  pname = "brook-d3d11-probe";
  version = "0.1";
  src = ./.;
  buildPhase = ''
    runHook preBuild
    $CXX -std=c++17 -O2 -Wall -Wextra -Werror -static \
      probe.cpp -ld3d11 -ldxgi -ld3dcompiler -ldxguid -lgdi32 -o brook-d3d11.exe
    runHook postBuild
  '';
  installPhase = ''
    mkdir -p $out/bin
    cp brook-d3d11.exe $out/bin/
  '';
}
