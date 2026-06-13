/* gltron launcher for Brook: setenv() the Mesa DRI + EGL-vendor paths (so the
 * unmodified Mesa loader finds virtio_gpu_dri.so and the Mesa EGL ICD), force
 * the virgl driver, point SDL2 at the Wayland backend with a dummy audio
 * driver, and give gltron a writable HOME for its .gltronrc. Then exec the real
 * gltron binary. Same static-env C shim pattern as es2gears/gl-probe; paths are
 * baked at build time via -D from the nix derivation.
 *
 * gltron uses sdl12-compat (libSDL-1.2 -> SDL2), so it drives the same hardware
 * zwp_linux_dmabuf_v1 presentation path as es2gears (PRIME export -> waylandd
 * import -> WM_PRESENT_GRES blit). */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#ifndef MESA_DRI_PATH
#define MESA_DRI_PATH "/run/opengl-driver/lib/dri"
#endif
#ifndef MESA_GBM_PATH
#define MESA_GBM_PATH "/run/opengl-driver/lib/gbm"
#endif
#ifndef MESA_EGL_VENDOR_DIR
#define MESA_EGL_VENDOR_DIR "/run/opengl-driver/share/glvnd/egl_vendor.d"
#endif
#ifndef GLTRON_REAL
#define GLTRON_REAL "/bin/gltron-real"
#endif

int main(int argc, char **argv)
{
    setenv("LIBGL_DRIVERS_PATH", MESA_DRI_PATH, 1);
    setenv("GBM_BACKENDS_PATH", MESA_GBM_PATH, 1);
    setenv("__EGL_VENDOR_LIBRARY_DIRS", MESA_EGL_VENDOR_DIR, 1);
    /* Force the virgl DRI driver (host nvidia ICD would break otherwise). */
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "virtio_gpu", 1);
    /* SDL2 (via sdl12-compat): Wayland video, no audio. Brook's SDL audio path
     * (sdl12-compat -> sdl2-compat -> SDL3 + libmikmod/SDL_sound) hangs while
     * decoding gltron's bundled .it module, so we run gltron with -s ("Don't
     * play sound", see below) and also pin the dummy audio driver across all
     * three SDL ABIs as belt-and-braces. The GL render path is unaffected. */
    setenv("SDL_VIDEODRIVER", "wayland", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);   /* SDL1.2 / SDL2 */
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);  /* SDL3 */
    /* gltron writes ~/.gltronrc; give it a writable HOME. */
    setenv("HOME", "/tmp", 1);

    char *args[16];
    int n = 0;
    args[n++] = (char *)GLTRON_REAL;
    args[n++] = (char *)"-s";   /* disable sound entirely (avoids the hang) */
    for (int i = 1; i < argc && n < 15; ++i) args[n++] = argv[i];
    args[n] = NULL;

    execv(GLTRON_REAL, args);
    perror("gltron: execv");
    return 1;
}
