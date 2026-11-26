{ pkgs ? import ../../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-input-preferences-fixture";
  version = "1";
  src = ./.;
  nativeBuildInputs = [ pkgs.pkg-config pkgs.wayland-scanner ];
  buildInputs = [ pkgs.wayland pkgs.libxkbcommon ];
  buildPhase = ''
    protocol=${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    wayland-scanner client-header "$protocol" xdg-shell-client.h
    wayland-scanner private-code "$protocol" xdg-shell.c
    viewport=${pkgs.wayland-protocols}/share/wayland-protocols/stable/viewporter/viewporter.xml
    fractional=${pkgs.wayland-protocols}/share/wayland-protocols/staging/fractional-scale/fractional-scale-v1.xml
    wayland-scanner client-header "$viewport" viewporter-client.h
    wayland-scanner private-code "$viewport" viewporter.c
    wayland-scanner client-header "$fractional" fractional-scale-client.h
    wayland-scanner private-code "$fractional" fractional-scale.c
    $CC -Wall -Wextra -Werror -O2 main.c xdg-shell.c viewporter.c fractional-scale.c \
      $(pkg-config --cflags --libs wayland-client xkbcommon) -o input-preferences-fixture
  '';
  installPhase = ''
    install -D input-preferences-fixture "$out/bin/input-preferences-fixture"
  '';
}
