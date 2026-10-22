{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-unix-credentials-probe";
  version = "0.1";
  src = ../at-syscall-probe;
  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -O2 -Wall -Wextra -Werror passcredcheck.c -o passcredcheck
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 passcredcheck $out/bin/passcredcheck
    runHook postInstall
  '';
}
