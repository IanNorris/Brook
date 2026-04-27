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
  fontsConf = ''
    <?xml version="1.0"?>
    <!DOCTYPE fontconfig SYSTEM "fonts.dtd">
    <fontconfig>
      <!-- Absolute paths so fontconfig finds the fonts regardless of how
           it computes prefixes; @OUT@ is substituted at install time. -->
      <dir>@OUT@/share/fonts/dejavu</dir>
      <cachedir>@OUT@/var/cache/fontconfig</cachedir>
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

    cat > $out/etc/fonts/fonts.conf <<EOF
${fontsConf}
EOF
    sed -i "s|@OUT@|$out|g" $out/etc/fonts/fonts.conf

    # Pre-build the cache so clients don't have to.
    # fc-cache writes caches into the <cachedir> declared in fonts.conf.
    # (--cache-dir is NOT a valid fc-cache flag; older code passed it and
    # then masked the error with `|| true`, leaving the cache empty and
    # forcing every client to do a full font scan at startup, which on
    # Brook silently produces missing-glyph fallbacks → tofu rendering →
    # huge GTK label widths → ftruncate overflow crashes.)
    FONTCONFIG_FILE=$out/etc/fonts/fonts.conf \
    FONTCONFIG_PATH=$out/etc/fonts \
      ${fontconfig.bin}/bin/fc-cache -v -f -s $out/share/fonts/dejavu

    echo "brook-fonts: $(ls $out/share/fonts/dejavu | wc -l) fonts, cache=$(ls $out/var/cache/fontconfig 2>/dev/null | wc -l) entries"
    # Sanity: fail the build if the cache wasn't actually written.
    if [ "$(ls $out/var/cache/fontconfig 2>/dev/null | wc -l)" = "0" ]; then
      echo "ERROR: fontconfig cache is empty after fc-cache" >&2
      exit 1
    fi
  '';
}
