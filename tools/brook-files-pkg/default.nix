{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, wayland ? (import ../../nix/nixpkgs.nix {}).wayland
, wayland-scanner ? (import ../../nix/nixpkgs.nix {}).wayland-scanner
, wayland-protocols ? (import ../../nix/nixpkgs.nix {}).wayland-protocols
, freetype ? (import ../../nix/nixpkgs.nix {}).freetype
, noto-fonts ? (import ../../nix/nixpkgs.nix {}).noto-fonts
, runCommand ? (import ../../nix/nixpkgs.nix {}).runCommand
, pkg-config ? (import ../../nix/nixpkgs.nix {}).pkg-config
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
}:

let
  monoFont = runCommand "brook-ui-mono-font" {} ''
    install -Dm444 ${noto-fonts}/share/fonts/noto/NotoSansMono.ttf $out/share/fonts/NotoSansMono.ttf
  '';
in stdenv.mkDerivation {
  pname = "brook-files-brook";
  version = "0.2-brook";

  src = ./.;

  nativeBuildInputs = [ patchelf wayland-scanner pkg-config ];
  buildInputs = [ wayland freetype ];

  buildPhase = ''
    XDG_XML=${wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    DECO_XML=${wayland-protocols}/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
    wayland-scanner client-header $XDG_XML xdg-shell-client-protocol.h
    wayland-scanner private-code  $XDG_XML xdg-shell-protocol.c
    wayland-scanner client-header $DECO_XML xdg-decoration-client-protocol.h
    wayland-scanner private-code  $DECO_XML xdg-decoration-protocol.c

    $CC -O2 -Wall -Wextra \
        -I${wayland.dev}/include -I. $(pkg-config --cflags freetype2) \
        -DBROOK_UI_FONT='"${monoFont}/share/fonts/NotoSansMono.ttf"' \
        brook-files.c xdg-shell-protocol.c xdg-decoration-protocol.c \
        -L${wayland}/lib -lwayland-client $(pkg-config --libs freetype2) \
        -Wl,-rpath,${wayland}/lib:${freetype}/lib:${stdenv.cc.libc}/lib \
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
