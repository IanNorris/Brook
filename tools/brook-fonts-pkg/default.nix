{ stdenv ? (import <nixpkgs> {}).stdenv
, dejavu_fonts ? (import <nixpkgs> {}).dejavu_fonts
, fontconfig ? (import <nixpkgs> {}).fontconfig
, writeText ? (import <nixpkgs> {}).writeText
}:

# Brook fonts bundle.
#
# Provides:
#   $out/share/fonts/dejavu/                 - DejaVu Sans + Sans Mono TTFs
#   $out/etc/fonts/fonts.conf                - minimal config pointing at above
#   $out/var/cache/fontconfig/               - pre-built cache (so first-run
#                                              clients don't need to write)
#
# Set FONTCONFIG_FILE in process env to point at $out/etc/fonts/fonts.conf
# and clickdot/eventdemo's window_frame_create() (cairo+pango+fontconfig)
# can find a sans-serif face.

let
  fontsConf = writeText "fonts.conf" ''
    <?xml version="1.0"?>
    <!DOCTYPE fontconfig SYSTEM "fonts.dtd">
    <fontconfig>
      <dir prefix="default">../../share/fonts/dejavu</dir>
      <cachedir prefix="default">../../var/cache/fontconfig</cachedir>
      <cachedir>/tmp/fontconfig</cachedir>

      <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
      <alias><family>serif</family>     <prefer><family>DejaVu Sans</family></prefer></alias>
      <alias><family>monospace</family> <prefer><family>DejaVu Sans Mono</family></prefer></alias>
      <alias><family>sans</family>      <prefer><family>DejaVu Sans</family></prefer></alias>

      <config><rescan><int>30</int></rescan></config>
    </fontconfig>
  '';
in
stdenv.mkDerivation {
  pname = "brook-fonts";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ fontconfig ];
  dontStrip = true;
  dontPatchELF = true;
  buildPhase = "true";
  installPhase = ''
    set -eux
    mkdir -p $out/share/fonts/dejavu $out/etc/fonts $out/var/cache/fontconfig

    # Copy a small useful subset of DejaVu (full set is ~10 MB; keep it tight)
    for f in DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSansMono.ttf; do
      cp ${dejavu_fonts}/share/fonts/truetype/$f $out/share/fonts/dejavu/
    done

    cp ${fontsConf} $out/etc/fonts/fonts.conf

    # Pre-build the cache so clients don't have to. fc-cache uses
    # FONTCONFIG_FILE if set, otherwise reads system fontconfig dirs.
    FONTCONFIG_FILE=$out/etc/fonts/fonts.conf \
    FONTCONFIG_PATH=$out/etc/fonts \
      ${fontconfig.bin}/bin/fc-cache -v -s -y $out/share/fonts/dejavu \
        --cache-dir=$out/var/cache/fontconfig || true

    echo "brook-fonts: $(ls $out/share/fonts/dejavu | wc -l) fonts, cache=$(ls $out/var/cache/fontconfig 2>/dev/null | wc -l) entries"
  '';
}
