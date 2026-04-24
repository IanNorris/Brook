{ stdenv ? (import <nixpkgs> {}).stdenv
, wayland ? (import <nixpkgs> {}).wayland
, patchelf ? (import <nixpkgs> {}).patchelf
}:

# Brook Wayland client smoke test — connects to waylandd, enumerates
# globals, exits.

stdenv.mkDerivation {
  pname = "wayland-smoke-brook";
  version = "0.1-brook";

  src = ./.;

  nativeBuildInputs = [ patchelf ];
  buildInputs = [ wayland ];

  buildPhase = ''
    $CC -O2 -Wall -Wextra \
        -I${wayland.dev}/include \
        wayland-smoke.c \
        -L${wayland}/lib -lwayland-client \
        -Wl,-rpath,${wayland}/lib:${stdenv.cc.libc}/lib \
        -o wayland-smoke
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 wayland-smoke $out/bin/wayland-smoke

    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/wayland-smoke

    strip $out/bin/wayland-smoke
  '';
}
