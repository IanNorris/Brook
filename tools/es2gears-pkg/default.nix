{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, mesa ? (import ../../nix/nixpkgs.nix {}).mesa
, mesa-demos ? (import ../../nix/nixpkgs.nix {}).mesa-demos
, libglvnd ? (import ../../nix/nixpkgs.nix {}).libglvnd
, libdrm ? (import ../../nix/nixpkgs.nix {}).libdrm
, wayland ? (import ../../nix/nixpkgs.nix {}).wayland
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
, makeWrapper ? (import ../../nix/nixpkgs.nix {}).makeWrapper
}:

# A minimal *windowed* Mesa GL client for Brook: es2gears_wayland from
# mesa-demos. Unlike gl-probe (surfaceless readback), this creates a real
# wl_surface + EGL window and eglSwapBuffers, exercising the hardware
# zwp_linux_dmabuf_v1 presentation path (PRIME export -> waylandd import ->
# WM_PRESENT_GRES blit). It is the smallest end-to-end test of hardware windowed
# GL before the heavier SuperTuxKart integration.
#
# Mesa's DRI driver (virtio_gpu_dri.so) and the EGL vendor JSON are pointed at
# via env (LIBGL_DRIVERS_PATH / __EGL_VENDOR_LIBRARY_DIRS) baked into a wrapper,
# matching how Brook runs unmodified Mesa from the nix store.

stdenv.mkDerivation {
  pname = "es2gears-brook";
  version = "0.1-brook";

  dontUnpack = true;
  nativeBuildInputs = [ patchelf makeWrapper ];
  buildInputs = [ mesa mesa-demos libglvnd libdrm wayland ];

  installPhase = ''
    mkdir -p $out/bin
    cp ${mesa-demos}/bin/es2gears_wayland $out/bin/.es2gears-real
    chmod +w $out/bin/.es2gears-real
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        --set-rpath "${libglvnd}/lib:${mesa}/lib:${libdrm}/lib:${wayland}/lib:${stdenv.cc.libc}/lib" \
        $out/bin/.es2gears-real

    # Wrapper bakes the Mesa DRI + EGL-vendor env so the binary is self-contained
    # (forces the Mesa ICD; the host nvidia ICD would break surfaceless EGL).
    makeWrapper $out/bin/.es2gears-real $out/bin/es2gears \
        --set LIBGL_DRIVERS_PATH "${mesa}/lib/dri" \
        --set GBM_BACKENDS_PATH "${mesa}/lib/gbm" \
        --set __EGL_VENDOR_LIBRARY_DIRS "${mesa}/share/glvnd/egl_vendor.d" \
        --prefix LD_LIBRARY_PATH : "${libglvnd}/lib:${mesa}/lib"
  '';
}
