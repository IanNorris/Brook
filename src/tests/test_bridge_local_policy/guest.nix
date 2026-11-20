let pkgs = import ../../../nix/nixpkgs.nix { };
in pkgs.pkgsStatic.stdenv.mkDerivation {
  pname = "brook-bridge-local-policy-test";
  version = "1";
  src = ./guest.c;
  dontUnpack = true;
  buildPhase = ''
    $CC -static -Wall -Wextra -Werror -O2 "$src" -o bridge-local-policy-test
  '';
  installPhase = ''
    mkdir -p "$out/bin"
    install -m755 bridge-local-policy-test "$out/bin/LOCBRDG.ELF"
  '';
}
