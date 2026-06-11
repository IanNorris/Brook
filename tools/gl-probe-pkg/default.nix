{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, mesa ? (import ../../nix/nixpkgs.nix {}).mesa
, libglvnd ? (import ../../nix/nixpkgs.nix {}).libglvnd
, libdrm ? (import ../../nix/nixpkgs.nix {}).libdrm
, libGL ? (import ../../nix/nixpkgs.nix {}).libGL
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
}:

# Brook GL shim M2-prep probe: a surfaceless EGL/GLES2 client that drives
# unmodified Mesa's virgl Gallium driver (virtio_gpu_dri.so) against Brook's
# /dev/dri/renderD128, far enough to create a GL context and submit draw
# commands. Run under Brook --strace to capture the DRM ioctl sequence Mesa
# needs (the M1 implementation spec).
#
# The Mesa DRI driver path + EGL vendor JSON are baked in via -D so the binary
# is self-contained (it setenv()s them before EGL init) — no Brook-side env
# plumbing required.

let
  driPath = "${mesa}/lib/dri";
  vendorJson = "${mesa}/share/glvnd/egl_vendor.d/50_mesa.json";
in
stdenv.mkDerivation {
  pname = "gl-probe-brook";
  version = "0.1-brook";

  src = ./.;

  nativeBuildInputs = [ patchelf ];
  buildInputs = [ mesa libglvnd libdrm libGL ];

  buildPhase = ''
    $CC -O2 -Wall -Wextra \
        -DMESA_DRI_PATH='"${driPath}"' \
        -DMESA_EGL_VENDOR='"${vendorJson}"' \
        -I${libglvnd.dev}/include \
        -I${mesa.dev or mesa}/include \
        gl-probe.c \
        -L${libglvnd}/lib -lEGL -lGLESv2 \
        -Wl,-rpath,${libglvnd}/lib:${mesa}/lib:${libdrm}/lib:${stdenv.cc.libc}/lib \
        -o gl-probe
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 gl-probe $out/bin/gl-probe

    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/gl-probe
  '';
}
