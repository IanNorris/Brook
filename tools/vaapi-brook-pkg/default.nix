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
  });
  driver = pkgs.intel-vaapi-driver.override { libdrm = libdrmBrook; };
in pkgs.buildEnv {
  name = "brook-vaapi-0.2";
  # ffmpeg-headless (libva-minimal, no X11/SDL) keeps the staged closure small
  # while keeping the VAAPI hwaccel.
  paths = [ pkgs.libva driver pkgs.libva-utils pkgs.ffmpeg-headless ];
  pathsToLink = [ "/bin" "/lib" "/share" ];
  passthru = { inherit driver libdrmBrook; ffmpeg = pkgs.ffmpeg-headless; libva = pkgs.libva; };
}
