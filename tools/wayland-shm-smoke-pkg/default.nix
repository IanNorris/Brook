{ stdenv ? (import <nixpkgs> {}).stdenv
, wayland ? (import <nixpkgs> {}).wayland
, patchelf ? (import <nixpkgs> {}).patchelf
}:

# Brook Wayland shm client smoke test — creates a wl_shm buffer, draws
# a test pattern, commits on a surface, and verifies end-to-end.

stdenv.mkDerivation {
  pname = "wayland-shm-smoke-brook";
  version = "0.1-brook";

  src = ./.;

  nativeBuildInputs = [ patchelf ];
  buildInputs = [ wayland ];

  buildPhase = ''
    $CC -O2 -Wall -Wextra \
        -I${wayland.dev}/include \
        wayland-shm-smoke.c \
        -L${wayland}/lib -lwayland-client \
        -Wl,-rpath,${wayland}/lib:${stdenv.cc.libc}/lib \
        -o wayland-shm-smoke
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 wayland-shm-smoke $out/bin/wayland-shm-smoke

    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/wayland-shm-smoke

    strip $out/bin/wayland-shm-smoke
  '';
}
