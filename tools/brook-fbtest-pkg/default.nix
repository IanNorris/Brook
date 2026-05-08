{ stdenv ? (import <nixpkgs> {}).stdenv
, wayland ? (import <nixpkgs> {}).wayland
, wayland-scanner ? (import <nixpkgs> {}).wayland-scanner
, wayland-protocols ? (import <nixpkgs> {}).wayland-protocols
, patchelf ? (import <nixpkgs> {}).patchelf
}:

# brook-fbtest: minimal framebuffer stress test — no ffmpeg, just
# renders a sweeping bar at target FPS to isolate display pipeline.

stdenv.mkDerivation {
  pname = "brook-fbtest";
  version = "0.1-brook";

  src = builtins.filterSource
    (path: type: baseNameOf path != "result" && baseNameOf path != ".git")
    ./.;

  nativeBuildInputs = [ patchelf wayland-scanner ];
  buildInputs = [ wayland ];

  buildPhase = ''
    XDG_XML=${wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    wayland-scanner client-header $XDG_XML xdg-shell-client-protocol.h
    wayland-scanner private-code  $XDG_XML xdg-shell-protocol.c

    $CC -O2 -Wall -Wextra \
        -I${wayland.dev}/include \
        -I. \
        brook-fbtest.c xdg-shell-protocol.c \
        -L${wayland}/lib -lwayland-client \
        -Wl,-rpath,${wayland}/lib:${stdenv.cc.libc}/lib \
        -o brook-fbtest
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 brook-fbtest $out/bin/brook-fbtest
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/brook-fbtest
  '';
}
