{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, wayland ? (import ../../nix/nixpkgs.nix {}).wayland
, wayland-scanner ? (import ../../nix/nixpkgs.nix {}).wayland-scanner
, wayland-protocols ? (import ../../nix/nixpkgs.nix {}).wayland-protocols
, ffmpeg ? (import ../../nix/nixpkgs.nix {}).ffmpeg
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
}:

# brook-player: minimal wl_shm video player using ffmpeg libav* decoding.
# Bypasses SDL entirely — renders decoded frames directly to Wayland shm buffers.

stdenv.mkDerivation {
  pname = "brook-player";
  version = "0.1-brook";

  src = builtins.filterSource
    (path: type: baseNameOf path != "result" && baseNameOf path != ".git")
    ./.;

  nativeBuildInputs = [ patchelf wayland-scanner ];
  buildInputs = [ wayland ffmpeg ];

  buildPhase = ''
    XDG_XML=${wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    wayland-scanner client-header $XDG_XML xdg-shell-client-protocol.h
    wayland-scanner private-code  $XDG_XML xdg-shell-protocol.c

    $CC -O2 -Wall -Wextra \
        -I${wayland.dev}/include \
        -I${ffmpeg.dev}/include \
        -I. \
        brook-player.c xdg-shell-protocol.c \
        -L${wayland}/lib -lwayland-client \
        -L${ffmpeg.lib}/lib -lavformat -lavcodec -lavutil -lswscale -lswresample \
        -Wl,-rpath,${wayland}/lib:${ffmpeg.lib}/lib:${stdenv.cc.libc}/lib \
        -lm \
        -o brook-player
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 brook-player $out/bin/brook-player
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/brook-player
    strip $out/bin/brook-player
  '';
}
