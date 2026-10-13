# Brook build of openal-soft with the OSS backend enabled.
#
# WHY: yquake2 uses OpenAL by default (cvar s_openal 1) and dlopen()s
# libopenal.so.1.  The stock nixpkgs openal-soft is built with only the pulse,
# alsa and wave backends — none of which Brook provides (there is no ALSA,
# PulseAudio, PipeWire or JACK server in the guest).  So on Brook, stock openal
# finds no usable device and yquake2 runs silent.
#
# Brook DOES expose a working OSS device: /dev/dsp implements the OSS ioctl
# surface (SNDCTL_DSP_SPEED/SETFMT/CHANNELS/GETOSPACE/GETFMTS/SETFRAGMENT/...)
# backed by the kernel audio mixer + Intel HDA driver.  A blocking write() to
# /dev/dsp paces on mixer free-space (the exact contract openal-soft's OSS
# backend relies on), and GETOSPACE reports truthful draining space.  The
# existing software-rendered quake2 port already drives audio through this
# device, so the kernel side is proven.
#
# This override rebuilds ONLY openal-soft with:
#   -DALSOFT_BACKEND_OSS=ON   enable the OSS backend
#   -DALSOFT_REQUIRE_OSS=ON   fail the build loudly if OSS support is missing,
#                             rather than silently dropping the backend
#
# The wrapper (yquake2-play.sh.in) then hard-sets ALSOFT_DRIVERS=oss and pins
# the output format to 44100/int16/stereo so Brook's mixer does no resample.
# Its lib dir is prepended to LD_LIBRARY_PATH so yquake2's bare-soname
# dlopen("libopenal.so.1") resolves to THIS build, not the stock closure one.

{ pkgs ? import ../../nix/nixpkgs.nix {} }:

pkgs.openal.overrideAttrs (old: {
  pname = "openal-soft-oss-brook";
  cmakeFlags = (old.cmakeFlags or []) ++ [
    "-DALSOFT_BACKEND_OSS=ON"
    "-DALSOFT_REQUIRE_OSS=ON"
  ];
})
