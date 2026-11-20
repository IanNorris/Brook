{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  required = [
    "chrome_display_list.h" "chrome_fonts.cpp" "chrome_material.h" "chrome_raster.cpp"
    "chrome_raster.h" "chrome_tray.h" "chrome_shell.cpp" "chrome_shell.h" "chrome_shell_model.h"
    "chrome_launcher.cpp" "chrome_launcher.h" "launcher_model.cpp" "launcher_model.h"
    "chrome_shell_wire.h" "chrome_style.h" "desktop_disk_popup.h" "desktop_stats_popup.h" "desktop_metrics.h" "desktop_telemetry.h"
    "font_atlas.h" "launcher_power.h" "taskbar_popup_pin.h" "taskbar_volume.h" "thermal_stats.h" "thermal_sampler.h"
  ];
  kernel = builtins.path {
    path = ../../src/kernel/src;
    name = "clearwater-builders";
    filter = path: type: type == "directory" || builtins.elem (baseNameOf path) required;
  };
  dbus = pkgs.pkgsStatic.dbus.override { enableSystemd = false; x11Support = false; };
  python = pkgs.python3.withPackages (p: [ p.freetype-py p.pillow p.cairosvg ]);
in pkgs.pkgsStatic.stdenv.mkDerivation {
  pname = "brook-clearwater-shell";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.dbus python ];
  buildInputs = [ dbus pkgs.pkgsStatic.libpng ];
  doCheck = true;
  checkPhase = "ctest --output-on-failure";
  postUnpack = ''
    mkdir -p brook/src/kernel brook/cmake brook/scripts
    cp -r ${kernel} brook/src/kernel/src
    ln -s ${../../src/shared} brook/src/shared
    cp ${../../cmake/ChromeFonts.cmake} brook/cmake/ChromeFonts.cmake
    cp ${../../scripts/bake_font.py} brook/scripts/bake_font.py
    cp ${../../scripts/bake_icons.py} brook/scripts/bake_icons.py
    export brookSource="$PWD/brook"
  '';
  preConfigure = ''
    cmakeFlagsArray+=(
      "-DBROOK_ROOT=$brookSource"
      "-DBROOK_TEST_DBUS_CONFIG=${pkgs.dbus}/share/dbus-1/session.conf"
      "-DBROOK_DESKTOP_FONT_TTF=${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf"
      "-DBROOK_MONO_FONT_TTF=${pkgs.hack-font}/share/fonts/truetype/Hack-Regular.ttf"
    )
  '';
}
