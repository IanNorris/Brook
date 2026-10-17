{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-gallery";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ pkgs.gtk3 ];
  buildPhase = ''
    $CC -O2 -Wall -Wextra -Werror gallery.c $(pkg-config --cflags --libs gtk+-3.0 gio-2.0) -o brook-gallery
  '';
  installPhase = ''
    mkdir -p $out/bin $out/share/applications $out/share/icons/hicolor/scalable/apps
    install -m755 brook-gallery $out/bin/
    cp brook-gallery.desktop $out/share/applications/
    cp ${../../data/icons/applications.svg} $out/share/icons/hicolor/scalable/apps/brook-gallery.svg
  '';
}
