#!/nix/store/4bwbk4an4bx7cb8xwffghvjjyfyl7m2i-bash-interactive-5.3p9/bin/bash
# Launch the nix-installed Yamagi Quake II (yquake2) on Brook's virgl GL stack.
# yquake2 is SDL2-based and dlopen()s its renderer (ref_gl1.so / ref_gl3.so /
# ref_gles3.so / ref_soft.so) from its own lib dir at runtime, picked by the
# `vid_renderer` cvar. We add the Brook virgl env (force the Mesa virtio_gpu DRI
# driver + glvnd EGL vendor dir, matching stk-play.sh / gltron-play.sh), SDL
# Wayland video, a dummy SDL audio backend (no audio device), a writable HOME for
# yquake2's ~/.yq2 config, and point basedir at the SHARED Quake II data that the
# native software port already uses (/data/games/quake2/baseq2/pak0.pak) so both
# renderers compare against identical assets.
#
# Renderer is overridable: this script defaults to gl1; pass a first arg of
# gl1 | gl3 | gles3 | soft to switch (e.g. the gl3 launch rc passes gl3).
export MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu
export LIBGL_DRIVERS_PATH=/nix/store/b793hmy0383vxaax7hiaslzmizzd3pmg-mesa-26.0.6/lib/dri
export GBM_BACKENDS_PATH=/nix/store/b793hmy0383vxaax7hiaslzmizzd3pmg-mesa-26.0.6/lib/gbm
export __EGL_VENDOR_LIBRARY_DIRS=/nix/store/b793hmy0383vxaax7hiaslzmizzd3pmg-mesa-26.0.6/share/glvnd/egl_vendor.d
export SDL_VIDEODRIVER=wayland
export ALSOFT_DRIVERS=null
export SDL_AUDIODRIVER=dummy
export HOME=/data/yq2
export XDG_CONFIG_HOME=/data/yq2/.config
export XDG_CACHE_HOME=/data/yq2/.cache
export XDG_DATA_HOME=/data/yq2/.local/share
mkdir -p /data/yq2/.config /data/yq2/.cache /data/yq2/.local/share

# First positional arg selects the renderer (default gl1); the rest pass through.
RENDERER="gl1"
case "${1:-}" in
    gl1|gl3|gles3|soft) RENDERER="$1"; shift ;;
esac

YQ2="$(command -v yquake2 2>/dev/null)"
[ -z "$YQ2" ] && YQ2=/nix/profile/bin/yquake2

# basedir points at the shared baseq2 data; vid_renderer selects GL vs software.
exec "$YQ2" +set basedir /data/games/quake2 +set vid_renderer "$RENDERER" "$@"
