{ pkgs ? import ../../../nix/nixpkgs.nix {} }:
let
  fixture = pkgs.stdenv.mkDerivation {
    name = "brook-pid-namespace-guest";
    src = ./guest.c;
    dontUnpack = true;
    buildPhase = ''$CC -O2 -Wall -Wextra -Werror "$src" -o pid-namespace-guest'';
    installPhase = ''install -Dm755 pid-namespace-guest "$out/bin/pid-namespace-guest"'';
  };
  seccomp = import ../test_seccomp/guest.nix { inherit pkgs; };
  packages = [fixture] ++ seccomp.packages;
in {
  inherit packages;
  profile = pkgs.buildEnv { name = "brook-pid-namespace-qemu-profile"; paths = packages; };
}
