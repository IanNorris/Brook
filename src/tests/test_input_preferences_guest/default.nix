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
    $CC -Wall -Wextra -Werror -O2 main.c xdg-shell.c \
      $(pkg-config --cflags --libs wayland-client xkbcommon) -o input-preferences-fixture
  '';
  installPhase = ''
    install -D input-preferences-fixture "$out/bin/input-preferences-fixture"
  '';
}
