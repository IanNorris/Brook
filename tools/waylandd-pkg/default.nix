{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, wayland ? (import ../../nix/nixpkgs.nix {}).wayland
, wayland-scanner ? (import ../../nix/nixpkgs.nix {}).wayland-scanner
, wayland-protocols ? (import ../../nix/nixpkgs.nix {}).wayland-protocols
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
}:

# Package the Brook Wayland server.
# Builds waylandd.c against libwayland-server from the Nix closure,
# generates xdg-shell server bindings via wayland-scanner, then
# patchelf's the result so the dynamic linker on Brook resolves
# wayland + libc out of the Nix disk image.

stdenv.mkDerivation {
  pname = "waylandd-brook";
  version = "0.1-brook";

  src = ./.;

  nativeBuildInputs = [ patchelf wayland-scanner ];
  buildInputs = [ wayland ];

  buildPhase = ''
    XDG_XML=${wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    DECO_XML=${wayland-protocols}/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
    VP_XML=${wayland-protocols}/share/wayland-protocols/stable/viewporter/viewporter.xml
    TI_XML=${wayland-protocols}/share/wayland-protocols/unstable/text-input/text-input-unstable-v3.xml
    wayland-scanner server-header  $XDG_XML  xdg-shell-server-protocol.h
    wayland-scanner private-code   $XDG_XML  xdg-shell-protocol.c
    wayland-scanner server-header  $DECO_XML xdg-decoration-server-protocol.h
    wayland-scanner private-code   $DECO_XML xdg-decoration-protocol.c
    wayland-scanner server-header  $VP_XML   viewporter-server-protocol.h
    wayland-scanner private-code   $VP_XML   viewporter-protocol.c
    wayland-scanner server-header  $TI_XML   text-input-unstable-v3-server-protocol.h
    wayland-scanner private-code   $TI_XML   text-input-unstable-v3-protocol.c

    $CC -O2 -Wall -Wextra \
        -I${wayland.dev}/include -I. \
        waylandd.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        text-input-unstable-v3-protocol.c \
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
