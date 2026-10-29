{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-message";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ pkgs.gtk3 ];
  postPatch = ''
    cp ${../../src/shared/brook_system_message.h} brook_system_message.h
    cp ${../../src/shared/brook_user_experience.h} brook_user_experience.h
    cp ${../../src/shared/brook_theme.h} brook_theme.h
    cp ${../../src/shared/brook_appearance.h} brook_appearance.h
    cp ${../../src/shared/brook_gtk_appearance.h} brook_gtk_appearance.h
  '';
  buildPhase = ''
    $CC -O2 -Wall -Wextra -Werror message.c message_format.c $(pkg-config --cflags --libs gtk+-3.0) -o brook-message
  '';
  doCheck = true;
  checkPhase = ''
    $CC -O2 -Wall -Wextra -Werror test_preferences.c $(pkg-config --cflags --libs glib-2.0) -o test-preferences
    ./test-preferences
    ./brook-message --code app-closed-memory --app GIMP --print
  '';
  installPhase = ''
    mkdir -p $out/bin
    install -m755 brook-message $out/bin/
  '';
}
