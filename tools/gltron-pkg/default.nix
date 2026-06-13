{ stdenv ? (import ../../nix/nixpkgs.nix {}).stdenv
, gltron ? (import ../../nix/nixpkgs.nix {}).gltron
, mesa ? (import ../../nix/nixpkgs.nix {}).mesa
, libglvnd ? (import ../../nix/nixpkgs.nix {}).libglvnd
, libdrm ? (import ../../nix/nixpkgs.nix {}).libdrm
, wayland ? (import ../../nix/nixpkgs.nix {}).wayland
, libxkbcommon ? (import ../../nix/nixpkgs.nix {}).libxkbcommon
, libdecor ? (import ../../nix/nixpkgs.nix {}).libdecor
, SDL2 ? (import ../../nix/nixpkgs.nix {}).SDL2
, patchelf ? (import ../../nix/nixpkgs.nix {}).patchelf
}:

# gltron for Brook: a real *windowed* OpenGL game (sdl12-compat -> SDL2 ->
# Wayland EGL), the first GL *application* (beyond the es2gears test) to exercise
# the hardware zwp_linux_dmabuf_v1 presentation path end to end. The stock gltron
# is an unmodified ELF whose nix closure already carries correct rpaths and the
# glibc-2.42 interpreter Brook uses, so we ship it verbatim plus a tiny C launcher
# that setenv()s the Mesa DRI + EGL-vendor paths and SDL backend then execs it.
# (mesa/libdecor/SDL2/wayland are listed as buildInputs so they are pinned into
# this package's runtime closure and get deployed to the nix disk.)

stdenv.mkDerivation {
  pname = "gltron-brook";
  version = "0.70-brook";

  src = ./.;
  nativeBuildInputs = [ patchelf ];
  buildInputs = [ gltron mesa libglvnd libdrm wayland libxkbcommon libdecor SDL2 ];

  buildPhase = ''
    $CC -O2 -Wall \
        -DMESA_DRI_PATH='"${mesa}/lib/dri"' \
        -DMESA_GBM_PATH='"${mesa}/lib/gbm"' \
        -DMESA_EGL_VENDOR_DIR='"${mesa}/share/glvnd/egl_vendor.d"' \
        -DGLTRON_REAL='"'$out'/bin/.gltron-real"' \
        launch.c -o gltron
  '';

  installPhase = ''
    mkdir -p $out/bin
    install -m 755 gltron $out/bin/gltron
    patchelf --set-interpreter "${stdenv.cc.libc}/lib/ld-linux-x86-64.so.2" \
        $out/bin/gltron

    # Ship the stock gltron unchanged (its rpath + interpreter are already
    # correct for Brook); just give it a private name behind the launcher.
    cp ${gltron}/bin/gltron $out/bin/.gltron-real
    chmod +w $out/bin/.gltron-real
  '';
}
