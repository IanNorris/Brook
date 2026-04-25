{ stdenv ? (import <nixpkgs> {}).stdenv
, weston ? (import <nixpkgs> {}).weston
, patchelf ? (import <nixpkgs> {}).patchelf
, glibc ? (import <nixpkgs> {}).glibc
}:

# Build a "fat" weston-flower package: copies the binary plus exactly the
# shared libraries it needs into a flat $out/lib, then rewrites every RPATH
# (and the interpreter) to point inside $out. The result is self-contained
# with a tiny nix-store closure (only $out itself + glibc for ld-linux).
stdenv.mkDerivation {
  pname = "weston-flower-bundle";
  version = "15.0.0-bundle";
  src = ./.;
  nativeBuildInputs = [ patchelf ];
  buildInputs = [ weston glibc ];
  buildPhase = "true";
  dontStrip = true;
  dontPatchELF = true;     # we patch manually, don't let nix mangle it

  installPhase = ''
    set -eux
    mkdir -p $out/bin $out/lib

    # Copy the demo binary.
    cp ${weston}/bin/weston-flower $out/bin/
    chmod +w $out/bin/weston-flower

    # Resolver: walks DT_NEEDED + RPATH transitively starting from the binary,
    # collects each lib's actual file path, copies into $out/lib.
    declare -A SEEN
    queue=("$out/bin/weston-flower")
    while [ ''${#queue[@]} -gt 0 ]; do
      cur="''${queue[0]}"
      queue=("''${queue[@]:1}")
      [ -n "''${SEEN[$cur]:-}" ] && continue
      SEEN[$cur]=1
      rp=$(patchelf --print-rpath "$cur" 2>/dev/null || echo "")
      for lib in $(patchelf --print-needed "$cur" 2>/dev/null); do
        IFS=":" read -ra rps <<< "$rp"
        for d in "''${rps[@]}"; do
          if [ -e "$d/$lib" ]; then
            if [ ! -e "$out/lib/$lib" ]; then
              cp -L "$d/$lib" "$out/lib/$lib"
              chmod +w "$out/lib/$lib"
              queue+=("$out/lib/$lib")
            fi
            break
          fi
        done
      done
    done

    # Rewrite RPATH on the binary and every copied lib to point only into $out/lib.
    # Set the interpreter to OUR copy of ld-linux (now living in $out/lib) so the
    # runtime closure of weston-flower is just $out itself — no glibc reference.
    patchelf --set-rpath "$out/lib" $out/bin/weston-flower
    if [ -e "$out/lib/ld-linux-x86-64.so.2" ]; then
      patchelf --set-interpreter "$out/lib/ld-linux-x86-64.so.2" $out/bin/weston-flower
    else
      cp -L ${glibc}/lib/ld-linux-x86-64.so.2 $out/lib/
      chmod +w $out/lib/ld-linux-x86-64.so.2
      patchelf --set-interpreter "$out/lib/ld-linux-x86-64.so.2" $out/bin/weston-flower
    fi
    for f in $out/lib/*.so*; do
      # Don't patch the dynamic linker itself — patchelf can corrupt its
      # static-PIE program headers, breaking _dl_start's self-relocation.
      case "$(basename "$f")" in
        ld-linux-x86-64.so.2|ld-linux.so.2|ld-2.*) continue;;
      esac
      patchelf --set-rpath "$out/lib" "$f" 2>/dev/null || true
    done

    echo "weston-flower bundle: $(ls $out/lib | wc -l) shared libs"
  '';
}
