{ stdenv ? (import <nixpkgs> {}).stdenv
, weston ? (import <nixpkgs> {}).weston
, patchelf ? (import <nixpkgs> {}).patchelf
, glibc ? (import <nixpkgs> {}).glibc
, binary ? "weston-eventdemo"
}:

# Generic single-binary weston-demo bundle.  Same structure as
# weston-flower-pkg: copy `binary` from upstream weston, walk DT_NEEDED,
# copy each shared lib into $out/lib, rewrite RPATHs and interpreter so
# the runtime closure is just $out itself.
stdenv.mkDerivation {
  pname = "weston-${binary}-bundle";
  version = "15.0.0-bundle";
  src = ./.;
  nativeBuildInputs = [ patchelf ];
  buildInputs = [ weston glibc ];
  buildPhase = "true";
  dontStrip = true;
  dontPatchELF = true;

  installPhase = ''
    set -eux
    mkdir -p $out/bin $out/lib

    cp ${weston}/bin/${binary} $out/bin/
    chmod +w $out/bin/${binary}

    declare -A SEEN
    queue=("$out/bin/${binary}")
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

    patchelf --set-rpath "$out/lib" $out/bin/${binary}
    if [ -e "$out/lib/ld-linux-x86-64.so.2" ]; then
      patchelf --set-interpreter "$out/lib/ld-linux-x86-64.so.2" $out/bin/${binary}
    else
      cp -L ${glibc}/lib/ld-linux-x86-64.so.2 $out/lib/
      chmod +w $out/lib/ld-linux-x86-64.so.2
      patchelf --set-interpreter "$out/lib/ld-linux-x86-64.so.2" $out/bin/${binary}
    fi
    for f in $out/lib/*.so*; do
      case "$(basename "$f")" in
        ld-linux-x86-64.so.2|ld-linux.so.2|ld-2.*) continue;;
      esac
      patchelf --set-rpath "$out/lib" "$f" 2>/dev/null || true
    done

    echo "${binary} bundle: $(ls $out/lib | wc -l) shared libs"
  '';
}
