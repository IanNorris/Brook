{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  windows = pkgs.pkgsCross.mingwW64;
in windows.stdenv.mkDerivation {
  pname = "brook-windows-snake";
  version = pkgs.sdl3.version;
  src = pkgs.sdl3.src;
  buildInputs = [ windows.sdl3 ];
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    $CC -O2 -Wall -Wextra examples/demo/01-snake/snake.c \
      -I${windows.sdl3.dev}/include -L${windows.sdl3.lib}/lib \
      -lSDL3 -o brook-snake.exe
    runHook postBuild
  '';
  installPhase = ''
    mkdir -p $out/bin $out/share/licenses
    cp brook-snake.exe ${windows.sdl3.out}/bin/SDL3.dll $out/bin/
    cp LICENSE.txt $out/share/licenses/SDL3.txt
  '';
}
