{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-settings";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ pkgs.gtk3 ];
  postPatch = ''
    cp ${../../src/shared/brook_theme.h} brook_theme.h
    cp ${../../src/shared/brook_audio_control.h} brook_audio_control.h
    cp ${../../src/shared/brook_appearance.h} brook_appearance.h
    cp ${../../src/shared/brook_user_experience.h} brook_user_experience.h
    cp ${../../src/shared/brook_experience_config.h} brook_experience_config.h
    cp ${../../src/shared/brook_taskbar.h} brook_taskbar.h
    cp ${../../src/shared/brook_pointer.h} brook_pointer.h
    cp ${../../src/shared/input_preferences.h} input_preferences.h
    cp ${../../src/shared/brook_desktop_scale.h} brook_desktop_scale.h
    cp ${../../src/shared/brook_gtk_appearance.h} brook_gtk_appearance.h
  '';
  buildPhase = ''
    $CC -O2 -Wall -Wextra -Werror settings.c $(pkg-config --cflags --libs gtk+-3.0) -o brook-settings
  '';
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    $CC -O2 -Wall -Wextra -Werror settings-persistence-test.c $(pkg-config --cflags --libs gtk+-3.0) -o settings-persistence-test
    ./settings-persistence-test
    runHook postCheck
  '';
  installPhase = ''
    mkdir -p $out/bin $out/share/applications $out/share/icons/hicolor/scalable/apps
    install -m755 brook-settings $out/bin/
    cp brook-settings.desktop $out/share/applications/
    cp ${../../data/icons/settings.svg} $out/share/icons/hicolor/scalable/apps/brook-settings.svg
  '';
}
