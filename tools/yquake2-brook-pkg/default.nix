# Brook build of Yamagi Quake II (yquake2) with Brook-specific client patches.
#
# This is the stock nixpkgs `yquake2` (the same build the GL3 path already runs)
# with ONE source patch applied — brook-features.patch — carrying two features:
#
#   1. Durable settings (BRO-... settings persistence): CL_WriteConfiguration now
#      writes config.cfg via a temp file + fsync + atomic rename, with a
#      re-entrancy guard. Brook's ext2 is not journaled and yquake2 is usually
#      killed the instant its window closes, so the stock fflush+fclose left the
#      freshly written settings in the block cache to be lost. The temp+rename
#      also makes a kill mid-write unable to truncate config.cfg.
#
#   2. Frametime overlay (cl_frametimegraph): a renderer-agnostic reimplementation
#      of the old software-port overlay (tools/quake2 swimp_brook.c) using
#      yquake2's own 2D Draw API (Draw_Fill / DrawStringScaled), so it works on
#      gl1/gl3/gles3/soft with no renderer changes. A 256-frame ring drawn as a
#      colour-coded bar graph (full scale 33 ms) plus an "FPS N  X.X ms" line,
#      gated on the archived cvar cl_frametimegraph.
#
# NOTE: this deliberately does NOT apply force-software-renderer.patch (see
# tools/yquake2-pkg) — that patch pins ref_soft and would break the GL3 path.
#
# Deps (SDL, mesa, wayland, openal) come from the same <nixpkgs> pin the rest of
# the staged closure was built against, so overriding only rebuilds yquake2.

{ pkgs ? import ../../nix/nixpkgs.nix {} }:

pkgs.yquake2.overrideAttrs (old: {
  pname = "yquake2-brook";
  patches = (old.patches or []) ++ [ ./brook-features.patch ];
})
