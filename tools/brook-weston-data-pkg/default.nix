{ stdenv ? (import <nixpkgs> {}).stdenv
, weston ? (import <nixpkgs> {}).weston
}:

# Brook bundle of weston's share/weston/ data files (PNG icons used by
# toytoolkit's frame_create() for close/minimize/maximize buttons).
#
# Without these, frame_create returns NULL and window_frame_create
# in turn returns NULL, crashing clients like weston-clickdot,
# weston-eventdemo and weston-terminal in widget_set_redraw_handler(NULL,...).
#
# Set WESTON_DATA_DIR=$out/share/weston in the process env to direct
# file_name_with_datadir() here.

stdenv.mkDerivation {
  pname = "brook-weston-data";
  version = "0.1";
  src = ./.;
  buildPhase = "true";
  dontStrip = true;
  dontPatchELF = true;
  installPhase = ''
    set -eux
    mkdir -p $out/share/weston
    cp -L ${weston}/share/weston/*.png $out/share/weston/
    echo "brook-weston-data: $(ls $out/share/weston | wc -l) files"
  '';
}
