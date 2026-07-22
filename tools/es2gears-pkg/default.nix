{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, mesa ? (import ../../nix/nixpkgs.nix {}).mesa
, mesa-demos ? (import ../../nix/nixpkgs.nix {}).mesa-demos
, libglvnd ? (import ../../nix/nixpkgs.nix {}).libglvnd
, libdrm ? (import ../../nix/nixpkgs.nix {}).libdrm
, wayland ? (import ../../nix/nixpkgs.nix {}).wayland
, libxkbcommon ? (import ../../nix/nixpkgs.nix {}).libxkbcommon
, libdecor ? (import ../../nix/nixpkgs.nix {}).libdecor
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
}:

# A minimal *windowed* Mesa GL client for Brook: es2gears_wayland from
# mesa-demos. Unlike gl-probe (surfaceless readback), this creates a real
# wl_surface + EGL window and eglSwapBuffers, exercising the hardware
# zwp_linux_dmabuf_v1 presentation path (PRIME export -> waylandd import ->
# WM_PRESENT_GRES blit). Smallest end-to-end test of hardware windowed GL
# before the heavier SuperTuxKart integration.
#
# We ship the real es2gears_wayland (rpath-locked to the Mesa/glvnd/wayland
# closure) plus a tiny C launcher that setenv()s the Mesa DRI + EGL vendor paths
# and execs it. The launcher (not a makeWrapper bash script) is what Brook's
# shell runs -- Brook can exec this static-env C shim directly but cannot
# nest-exec a bash wrapper.

stdenv.mkDerivation {
  pname = "es2gears-brook";
  version = "0.1-brook";

  src = ./.;
  nativeBuildInputs = [ patchelf ];
  buildInputs = [ mesa mesa-demos libglvnd libdrm wayland libxkbcommon libdecor ];

  buildPhase = ''
    $CC -O2 -Wall \
        -DMESA_DRI_PATH='"${mesa}/lib/dri"' \
        -DMESA_GBM_PATH='"${mesa}/lib/gbm"' \
        -DMESA_EGL_VENDOR_DIR='"${mesa}/share/glvnd/egl_vendor.d"' \
        -DES2GEARS_REAL='"'$out'/bin/.es2gears-real"' \
        launch.c -o es2gears
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 es2gears $out/bin/es2gears
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/es2gears

    cp ${mesa-demos}/bin/es2gears_wayland $out/bin/.es2gears-real
    chmod +w $out/bin/.es2gears-real
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        --set-rpath "${libglvnd}/lib:${mesa}/lib:${libdrm}/lib:${wayland}/lib:${libxkbcommon}/lib:${libdecor}/lib:${stdenv.cc.libc}/lib" \
        $out/bin/.es2gears-real
  '';
}
