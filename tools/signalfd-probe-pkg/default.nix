{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-signalfd-probe";
  version = "0.1";
  src = ./.;
  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -O2 -Wall -Wextra -Werror -pthread probe.c -o signalfd-probe
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 signalfd-probe $out/bin/signalfd-probe
    runHook postInstall
  '';
}
