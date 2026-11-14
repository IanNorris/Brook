# VA-API userspace for the Dell's Broadwell GT2 (VA-API phase 2, docs/bdw-video-engine.md).
# Opt-in and not part of any boot image: staged like the Iris package, then run
# with LIBVA_DRIVER_NAME=i965 LIBVA_DRIVERS_PATH=$out/lib/dri.
# The legacy intel-vaapi-driver (i965) supports Gen8; its libdrm_intel relocates
# every BO, which Brook's execbuffer refuses until the softpin/NO_RELOC patch
# (phase 2a) is applied on top of this expression.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  driver = pkgs.intel-vaapi-driver;
in pkgs.buildEnv {
  name = "brook-vaapi-0.1";
  paths = [ pkgs.libva driver pkgs.libva-utils pkgs.ffmpeg ];
  pathsToLink = [ "/bin" "/lib" "/share" ];
  passthru = { inherit driver; ffmpeg = pkgs.ffmpeg; libva = pkgs.libva; };
}
