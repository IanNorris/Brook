{ stdenv ? (import <nixpkgs> {}).stdenv
, expat ? (import <nixpkgs> {}).expat
, zlib ? (import <nixpkgs> {}).zlib
, patchelf ? (import <nixpkgs> {}).patchelf
}:

# Package the pre-built NetSurf framebuffer binary for Brook OS.
# The binary is built from tools/netsurf-build/ using make, then
# this derivation wraps it with proper Nix store references so
# the full closure can be copied to Brook's nix disk.

stdenv.mkDerivation {
  pname = "netsurf-brook";
  version = "3.11-brook";

  # No source fetch — we install the pre-built binary directly.
  dontUnpack = true;
  dontBuild = true;

  nativeBuildInputs = [ patchelf ];
  buildInputs = [ expat zlib ];

  installPhase = ''
    mkdir -p $out/bin $out/share/netsurf

    # Copy the pre-built binary (must be writable for patchelf)
    install -m 755 ${../netsurf-build/netsurf/nsfb} $out/bin/netsurf

    # Copy shared resources (CSS, HTML, images) from the common resources dir
    cp -r ${../netsurf-build/netsurf/resources}/* $out/share/netsurf/
    chmod -R u+w $out/share/netsurf

    # Copy framebuffer-specific resources (fonts, icons, pointers, Messages)
    # These may overlap with resources/ dirs, so merge carefully
    for item in fonts icons pointers; do
      if [ -d "${../netsurf-build/netsurf/frontends/framebuffer/res}/$item" ]; then
        cp -rn "${../netsurf-build/netsurf/frontends/framebuffer/res}/$item" $out/share/netsurf/ 2>/dev/null || true
      fi
    done

    # Per-language Messages files (not in resources/)
    for lang in ${../netsurf-build/netsurf/frontends/framebuffer/res}/*/Messages; do
      if [ -f "$lang" ]; then
        dir=$(basename $(dirname "$lang"))
        mkdir -p $out/share/netsurf/$dir
        cp -f "$lang" $out/share/netsurf/$dir/
      fi
    done
    # Top-level Messages
    cp -f ${../netsurf-build/netsurf/frontends/framebuffer/res}/en/Messages \
      $out/share/netsurf/Messages 2>/dev/null || true

    # Fix RUNPATH and interpreter to point to Nix store dependencies
    patchelf --set-rpath "${expat}/lib:${zlib}/lib:${stdenv.cc.libc}/lib" \
      $out/bin/netsurf
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
      $out/bin/netsurf

    # Strip debug info to save space on Brook's disk
    strip $out/bin/netsurf
  '';
}
