{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, wayland ? (import ../../nix/nixpkgs.nix {}).wayland
, wayland-scanner ? (import ../../nix/nixpkgs.nix {}).wayland-scanner
, wayland-protocols ? (import ../../nix/nixpkgs.nix {}).wayland-protocols
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
, xkbcomp ? (import ../../nix/nixpkgs.nix {}).xorg.xkbcomp
, libxkbfile ? (import ../../nix/nixpkgs.nix {}).xorg.libxkbfile
, libX11 ? (import ../../nix/nixpkgs.nix {}).xorg.libX11
, libxcb ? (import ../../nix/nixpkgs.nix {}).libxcb
, xorgserver ? (import ../../nix/nixpkgs.nix {}).xorg.xorgserver
, python3 ? (import ../../nix/nixpkgs.nix {}).python3
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

  postUnpack = ''
    cp ${../../src/shared/surface_geometry.h} "$sourceRoot/surface-geometry.h"
    cp ${../../src/shared/dmabuf_layout.h} "$sourceRoot/dmabuf-layout.h"
    cp ${../../src/shared/wm_window_state.h} "$sourceRoot/wm-window-state.h"
    cp ${../../src/kernel/src/gpu_scene_abi.h} "$sourceRoot/gpu-scene-abi.h"
  '';

  nativeBuildInputs = [ patchelf wayland-scanner ];
  buildInputs = [ wayland libxcb ];
  nativeCheckInputs = [ xkbcomp xorgserver python3 ];
  checkInputs = [ libxkbfile libX11 ];
  doCheck = true;

  checkPhase = ''
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        display-scale-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o display-scale-test
    ./display-scale-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        pointer-constraints-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o pointer-constraints-test
    ./pointer-constraints-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${libxkbfile.dev}/include -I${libX11.dev}/include \
        keyboard-keymap-test.c -L${libxkbfile}/lib -L${libX11}/lib \
        -lxkbfile -lX11 -o keyboard-keymap-test
    ./keyboard-keymap-test ${xkbcomp}/bin/xkbcomp
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        output-identity-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o output-identity-test
    ./output-identity-test
    $CC -O2 -UNDEBUG -Wall -Wextra surface-tree-test.c -o surface-tree-test
    ./surface-tree-test
    $CC -O2 -Wall -Wextra -I${wayland.dev}/include -I. \
        text-input-test.c text-input-v3-protocol.c \
        -L${wayland}/lib -lwayland-server -o text-input-test
    ./text-input-test
    $CC -O2 -Wall -Wextra -I${wayland.dev}/include -I. \
        surface-frame-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o surface-frame-test
    ./surface-frame-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        retained-surface-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o retained-surface-test
    ./retained-surface-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        dmabuf-import-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o dmabuf-import-test
    ./dmabuf-import-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        dmabuf-params-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o dmabuf-params-test
    ./dmabuf-params-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        configure-surface-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -lwayland-client -o configure-surface-test
    ./configure-surface-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        scene-surface-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o scene-surface-test
    ./scene-surface-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        viewport-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o viewport-test
    ./viewport-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        capture-release-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o capture-release-test
    ./capture-release-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        xwayland-shell-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o xwayland-shell-test
    ./xwayland-shell-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${wayland.dev}/include -I. \
        xwayland-bridge-test.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server -o xwayland-bridge-test
    ./xwayland-bridge-test
    $CC -O2 -UNDEBUG -Wall -Wextra xwm-wire-test.c -o xwm-wire-test
    ./xwm-wire-test
    $CC -O2 -UNDEBUG -Wall -Wextra -I${libxcb.dev}/include xwm-helper-test.c \
        -L${libxcb}/lib -lxcb -o xwm-helper-test
    python3 xwm-helper-check.py --xvfb ${xorgserver}/bin/Xvfb \
        --helper "$PWD/brook-xwm" --fixture "$PWD/xwm-helper-test"
  '';

  buildPhase = ''
    XDG_XML=${wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    DECO_XML=${wayland-protocols}/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
    VP_XML=${wayland-protocols}/share/wayland-protocols/stable/viewporter/viewporter.xml
    DMABUF_XML=${wayland-protocols}/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
    TEXT_XML=${wayland-protocols}/share/wayland-protocols/unstable/text-input/text-input-unstable-v3.xml
    RELATIVE_XML=${wayland-protocols}/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml
    CONSTRAINTS_XML=${wayland-protocols}/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
    wayland-scanner server-header $RELATIVE_XML relative-pointer-v1-server-protocol.h
    wayland-scanner private-code $RELATIVE_XML relative-pointer-v1-protocol.c
    wayland-scanner server-header $CONSTRAINTS_XML pointer-constraints-v1-server-protocol.h
    wayland-scanner private-code $CONSTRAINTS_XML pointer-constraints-v1-protocol.c
    XWAYLAND_XML=${wayland-protocols}/share/wayland-protocols/staging/xwayland-shell/xwayland-shell-v1.xml
    wayland-scanner server-header $XWAYLAND_XML xwayland-shell-v1-server-protocol.h
    wayland-scanner private-code $XWAYLAND_XML xwayland-shell-v1-protocol.c
    wayland-scanner server-header $TEXT_XML text-input-v3-server-protocol.h
    wayland-scanner private-code $TEXT_XML text-input-v3-protocol.c
    wayland-scanner server-header  $XDG_XML  xdg-shell-server-protocol.h
    wayland-scanner private-code   $XDG_XML  xdg-shell-protocol.c
    wayland-scanner server-header  $DECO_XML xdg-decoration-server-protocol.h
    wayland-scanner private-code   $DECO_XML xdg-decoration-protocol.c
    wayland-scanner server-header  protocols/server-decoration.xml kde-server-decoration-server-protocol.h
    wayland-scanner private-code   protocols/server-decoration.xml kde-server-decoration-protocol.c
    wayland-scanner server-header  $VP_XML   viewporter-server-protocol.h
    wayland-scanner private-code   $VP_XML   viewporter-protocol.c
    wayland-scanner server-header  $DMABUF_XML linux-dmabuf-unstable-v1-server-protocol.h
    wayland-scanner private-code   $DMABUF_XML linux-dmabuf-unstable-v1-protocol.c

    $CC -O2 -Wall -Wextra \
        -I${wayland.dev}/include -I. \
        waylandd.c xdg-shell-protocol.c xdg-decoration-protocol.c viewporter-protocol.c \
        linux-dmabuf-unstable-v1-protocol.c kde-server-decoration-protocol.c text-input-v3-protocol.c xwayland-shell-v1-protocol.c relative-pointer-v1-protocol.c pointer-constraints-v1-protocol.c \
        -L${wayland}/lib -lwayland-server \
        -Wl,-rpath,${wayland}/lib:${stdenv.cc.libc}/lib \
        -o waylandd
    $CC -O2 -Wall -Wextra -I${libxcb.dev}/include xwm-helper.c \
        -L${libxcb}/lib -lxcb -lxcb-composite -o brook-xwm
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 waylandd $out/bin/waylandd
    install -m 755 brook-xwm $out/bin/brook-xwm
    mkdir -p $out/share/licenses/waylandd-brook
    install -m 644 protocols/LGPL-2.1-or-later.txt protocols/README.md \
        $out/share/licenses/waylandd-brook/

    # Ensure the dynamic loader is explicit (so Brook's /lib/ld-linux
    # path doesn't matter — loader comes from the Nix store closure).
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/waylandd

    strip $out/bin/waylandd
  '';
}
