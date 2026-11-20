{ pkgs ? import ../../../nix/nixpkgs.nix {} }:
let
  test = pkgs.stdenv.mkDerivation {
    name = "brook-seccomp-guest";
    src = ./guest.c;
    dontUnpack = true;
    buildInputs = [ pkgs.libseccomp ];
    buildPhase = ''$CC -O2 -Wall -Wextra -Werror -pthread "$src" -lseccomp -o seccomp-guest'';
    installPhase = ''install -Dm755 seccomp-guest "$out/bin/seccomp-guest"'';
  };
  packages = [
    test pkgs.bash pkgs.coreutils pkgs.curl.bin pkgs.cacert pkgs.xz.bin pkgs.zstd.bin pkgs.bubblewrap
  ];
in {
  inherit packages;
  profile = pkgs.buildEnv { name = "brook-seccomp-qemu-profile"; paths = packages; };
}
