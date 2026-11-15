# VA-API userspace for the Dell's Broadwell GT2 (VA-API phase 2, docs/bdw-video-engine.md).
# Opt-in and not part of any boot image: staged like the Iris package, then run
# with LIBVA_DRIVER_NAME=i965 LIBVA_DRIVERS_PATH=$out/lib/dri.
#
# Brook's execbuffer refuses relocations, so the legacy intel-vaapi-driver
# (i965, Gen8) is built against a patched libdrm_intel that softpins every BO
# at a per-process GPU VA and submits with I915_EXEC_NO_RELOC
# (libdrm-intel-brook-softpin.patch). Only i965 sees the patched libdrm:
# libva, ffmpeg and everything else keep stock libdrm. The patch also works
# on Linux i915 (softpin is standard UAPI), so the same driver can be checked
# on a Linux host. BROOK_LIBDRM_SOFTPIN=0 restores stock relocation behaviour;
# BROOK_LIBDRM_SOFTPIN_48B=1 places every BO above 4 GiB.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  libdrmBrook = pkgs.libdrm.overrideAttrs (old: {
    pname = "libdrm-brook-softpin";
    patches = (old.patches or []) ++ [ ./libdrm-intel-brook-softpin.patch ];
    # Also build the static archives: i965 links libdrm_intel statically.
    mesonFlags = (old.mesonFlags or []) ++ [ "-Ddefault_library=both" ];
  });
  # i965 carries the softpin libdrm_intel inside itself, with its symbols made
  # local (--exclude-libs). A process can already hold stock libdrm_intel.so.1
  # before libva loads i965 (Mesa's libgallium links it, so every GL/EGL
  # client such as Chromium's GPU process does); the dynamic loader would then
  # satisfy i965's DT_NEEDED with that object, or bind its drm_intel_* calls
  # to it, and every execbuffer would carry relocations Brook refuses.
  # i965-brook-export-alloc.patch: allocate a surface's BO in
  # vaExportSurfaceHandle (Chromium exports its frame pool before decoding).
  driver = (pkgs.intel-vaapi-driver.override { libdrm = libdrmBrook; }).overrideAttrs (old: {
    patches = (old.patches or []) ++ [ ./i965-brook-export-alloc.patch ];
    preConfigure = (old.preConfigure or "") + ''
      mkdir -p brook-pc
      sed -e 's|^Libs:.*|Libs: ${libdrmBrook}/lib/libdrm_intel.a -Wl,--exclude-libs,libdrm_intel.a -lpciaccess|' \
          -e 's|^Requires.private:|Requires:|' \
          ${libdrmBrook.dev}/lib/pkgconfig/libdrm_intel.pc > brook-pc/libdrm_intel.pc
      export PKG_CONFIG_PATH="$PWD/brook-pc''${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    '';
    buildInputs = (old.buildInputs or []) ++ [ pkgs.libpciaccess ];
    # Fail the build if i965 still depends on a shared libdrm_intel.
    postFixup = (old.postFixup or "") + ''
      if readelf -d $out/lib/dri/i965_drv_video.so | grep -q 'libdrm_intel'; then
        echo "i965 still links libdrm_intel dynamically" >&2; exit 1
      fi
      if nm -D --defined-only $out/lib/dri/i965_drv_video.so | grep -q ' drm_intel_'; then
        echo "i965 exports drm_intel_ symbols" >&2; exit 1
      fi
    '';
  });
in pkgs.buildEnv {
  name = "brook-vaapi-0.3";
  # ffmpeg-headless (libva-minimal, no X11/SDL) keeps the staged closure small
  # while keeping the VAAPI hwaccel.
  paths = [ pkgs.libva driver pkgs.libva-utils pkgs.ffmpeg-headless ];
  pathsToLink = [ "/bin" "/lib" "/share" ];
  passthru = { inherit driver libdrmBrook; ffmpeg = pkgs.ffmpeg-headless; libva = pkgs.libva; };
}
