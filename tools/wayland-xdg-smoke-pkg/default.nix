{ stdenv ? (import <nixpkgs> {}).stdenv
, wayland ? (import <nixpkgs> {}).wayland
, wayland-scanner ? (import <nixpkgs> {}).wayland-scanner
, wayland-protocols ? (import <nixpkgs> {}).wayland-protocols
, patchelf ? (import <nixpkgs> {}).patchelf
}:

stdenv.mkDerivation {
  pname = "wayland-xdg-smoke-brook";
  version = "0.1-brook";

  src = ./.;

  nativeBuildInputs = [ patchelf wayland-scanner ];
  buildInputs = [ wayland ];

  buildPhase = ''
    XDG_XML=${wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    wayland-scanner client-header $XDG_XML xdg-shell-client-protocol.h
    wayland-scanner private-code  $XDG_XML xdg-shell-protocol.c

    $CC -O2 -Wall -Wextra \
        -I${wayland.dev}/include -I. \
        wayland-xdg-smoke.c xdg-shell-protocol.c \
        -L${wayland}/lib -lwayland-client \
        -Wl,-rpath,${wayland}/lib:${stdenv.cc.libc}/lib \
        -o wayland-xdg-smoke
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 wayland-xdg-smoke $out/bin/wayland-xdg-smoke
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/wayland-xdg-smoke
    strip $out/bin/wayland-xdg-smoke
  '';
}
