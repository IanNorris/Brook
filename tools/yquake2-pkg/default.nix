# Brook build of Yamagi Quake II (yquake2).
#
# This is the stock nixpkgs `yquake2` with a single Brook-specific patch applied
# to its software renderer (ref_soft). See force-software-renderer.patch:
#
#   yquake2's ref_soft (RE_InitContext, src/client/refresh/soft/sw_main.c) asks
#   SDL for a HARDWARE-ACCELERATED renderer to present its software-rasterised
#   frame (SDL_CreateRenderer with SDL_RENDERER_ACCELERATED). On a normal desktop
#   that call returns NULL when no GPU is available and yquake2 falls back to
#   SDL_RENDERER_SOFTWARE. On Brook's no-GPU wl_shm Wayland path the accelerated
#   attempt instead spins up SDL3's GL renderer, which builds a wl_egl window
#   whose backing wl_surface is invalid and crashes inside Mesa's EGL
#   (loader_wayland_wrap_surface -> wl_proxy_create_wrapper) with a user #PF
#   (BRO-204). Because it crashes rather than returning NULL, the software
#   fallback never runs.
#
#   The patch forces the software renderer directly on the SDL2 code path (the
#   one this build compiles — yquake2 8.60 links SDL2 via sdl2-compat), so
#   ref_soft presents through wl_shm and never touches EGL/GL.
#
# Deps (SDL, mesa, wayland, openal) come from the same <nixpkgs> the rest of the
# staged closure was built against, so overriding only rebuilds yquake2 itself
# and reuses every existing store path.

{ pkgs ? import <nixpkgs> {} }:

pkgs.yquake2.overrideAttrs (old: {
  pname = "yquake2-brook";
  patches = (old.patches or []) ++ [
    ./force-software-renderer.patch
    # Always-on Brook-style frametime bar graph (SCR_DrawFrameGraph). yquake2 is
    # a diagnostic lever here, so frame pacing is surfaced unconditionally.
    ./frametime-graph.patch
  ];
})
