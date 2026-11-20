let pkgs = import ../../../nix/nixpkgs.nix { };
in pkgs.pkgsStatic.stdenv.mkDerivation {
  pname = "brook-boot-storage-test";
  version = "1";
  src = ./guest.c;
  dontUnpack = true;
  buildPhase = ''
    $CC -static -Wall -Wextra -Werror -O2 "$src" -o boot-storage-test
  '';
  installPhase = ''
    mkdir -p "$out/bin"
    install -m755 boot-storage-test "$out/bin/ESPTEST.ELF"
  '';
}
