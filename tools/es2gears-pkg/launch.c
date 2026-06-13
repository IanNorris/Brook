/* es2gears launcher for Brook: setenv() the Mesa DRI + EGL-vendor paths (so the
 * unmodified Mesa loader finds virtio_gpu_dri.so and the Mesa EGL ICD), then
 * exec the real es2gears_wayland binary. This replaces a makeWrapper bash script
 * — Brook's shell can't nest-exec the bash wrapper, but it runs this static-env
 * C shim directly (the same approach as gl-probe). Paths are baked at build time
 * via -D from the nix derivation. */
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
#ifndef ES2GEARS_REAL
#define ES2GEARS_REAL "/bin/es2gears_wayland"
#endif

int main(int argc, char **argv)
{
    setenv("LIBGL_DRIVERS_PATH", MESA_DRI_PATH, 1);
    setenv("GBM_BACKENDS_PATH", MESA_GBM_PATH, 1);
    setenv("__EGL_VENDOR_LIBRARY_DIRS", MESA_EGL_VENDOR_DIR, 1);
    /* Force the virgl DRI driver (host nvidia ICD would break otherwise). */
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "virtio_gpu", 1);

    char *args[16];
    int n = 0;
    args[n++] = (char *)ES2GEARS_REAL;
    for (int i = 1; i < argc && n < 15; ++i) args[n++] = argv[i];
    args[n] = NULL;

    execv(ES2GEARS_REAL, args);
    perror("es2gears: execv");
    return 1;
}
