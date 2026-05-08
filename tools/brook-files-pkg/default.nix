{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, wayland ? (import ../../nix/nixpkgs.nix {}).wayland
, wayland-scanner ? (import ../../nix/nixpkgs.nix {}).wayland-scanner
, wayland-protocols ? (import ../../nix/nixpkgs.nix {}).wayland-protocols
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
}:

stdenv.mkDerivation {
  pname = "brook-files-brook";
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
        brook-files.c xdg-shell-protocol.c \
        -L${wayland}/lib -lwayland-client \
        -Wl,-rpath,${wayland}/lib:${stdenv.cc.libc}/lib \
        -o brook-files
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 brook-files $out/bin/brook-files
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/brook-files
    strip $out/bin/brook-files
  '';
}
