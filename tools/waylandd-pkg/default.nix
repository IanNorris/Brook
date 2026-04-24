{ stdenv ? (import <nixpkgs> {}).stdenv
, wayland ? (import <nixpkgs> {}).wayland
, patchelf ? (import <nixpkgs> {}).patchelf
}:

# Package the Brook "first-light" Wayland server.
# Builds waylandd.c against libwayland-server from the Nix closure,
# then patchelf's the result so the dynamic linker on Brook resolves
# wayland + libc out of the Nix disk image.

stdenv.mkDerivation {
  pname = "waylandd-brook";
  version = "0.1-brook";

  src = ./.;

  nativeBuildInputs = [ patchelf ];
  buildInputs = [ wayland ];

  buildPhase = ''
    $CC -O2 -Wall -Wextra \
        -I${wayland.dev}/include \
        waylandd.c \
        -L${wayland}/lib -lwayland-server \
        -Wl,-rpath,${wayland}/lib:${stdenv.cc.libc}/lib \
        -o waylandd
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 waylandd $out/bin/waylandd

    # Ensure the dynamic loader is explicit (so Brook's /lib/ld-linux
    # path doesn't matter — loader comes from the Nix store closure).
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/waylandd

    strip $out/bin/waylandd
  '';
}
