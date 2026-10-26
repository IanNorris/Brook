{ pkgs ? import ../../nix/nixpkgs.nix {} }:
{
  windows = pkgs.pkgsCross.mingwW64.stdenv.mkDerivation {
    pname = "brook-windows-steam-api-probe"; version = "0.1"; src = ./.;
    buildPhase = ''
      $CC -std=c11 -O2 -Wall -Wextra -Werror -static -D__USE_MINGW_ANSI_STDIO=0 windows.c -o steam-api-probe.exe
    '';
    installPhase = ''mkdir -p $out/bin; cp steam-api-probe.exe $out/bin/'';
  };
  linux = pkgs.stdenv.mkDerivation {
    pname = "brook-native-steam-api-probe"; version = "0.1"; src = ./.;
    buildPhase = ''
      $CC -std=c11 -O2 -Wall -Wextra -Werror linux.c -ldl -o steam-native-probe
    '';
    installPhase = ''mkdir -p $out/bin; cp steam-native-probe $out/bin/'';
  };
}
