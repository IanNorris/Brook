# Narrow XInput metadata refresh for the pinned SDL3 backend beneath SDL2.
# A master pointer's valuator modes change when Xwayland switches sources.
# See https://github.com/libsdl-org/SDL/issues/16163 and the upstream
# https://github.com/libsdl-org/SDL/pull/16267 . This focused backport updates
# cached relative axes from the event's copied scalar classes, without querying
# a newer server state or retaining pointers into the event. The first absolute
# sample after a metadata change only establishes a new coordinate baseline.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
let
  sdl3 = pkgs.sdl3.overrideAttrs (old: {
    patches = (old.patches or []) ++ [ ./xinput-device-change.patch ];
    nativeCheckInputs = (old.nativeCheckInputs or []) ++ [ pkgs.python3 ];
    postCheck = (old.postCheck or "") + ''
      python3 ${./xinput-axis-test.py} --source "$NIX_BUILD_TOP/$sourceRoot"
    '';
  });
in pkgs.SDL2.override { inherit sdl3; }
