{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-sysv-sem-probe";
  version = "0.1";
  src = ./.;
  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -O2 -Wall -Wextra -Werror probe.c -o sysv-sem-probe
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 sysv-sem-probe $out/bin/sysv-sem-probe
    runHook postInstall
  '';
}
