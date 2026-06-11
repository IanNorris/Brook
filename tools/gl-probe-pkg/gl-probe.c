// gl-probe.c — surfaceless EGL/GL probe for Brook's virtio-gpu DRM render node.
//
// Purpose (GL shim M2-prep): drive UNMODIFIED Mesa (libEGL + the virgl Gallium
// driver virtio_gpu_dri.so) against Brook's /dev/dri/renderD128, far enough to
// create a GL context and issue real draw commands. Run under Brook's --strace
// so the exact DRM ioctl sequence Mesa needs (CONTEXT_INIT, RESOURCE_CREATE,
// MAP, TRANSFER_*, EXECBUFFER, …) is captured — that becomes the M1 spec.
//
// Self-contained: the Mesa driver path + EGL vendor JSON + driver override are
// baked in at build time (-D… from the nix derivation) and applied via setenv()
// before EGL init, so no run-time environment plumbing is needed in Brook.
//
// Every step logs to stderr with a PROBE_ prefix; prints PROBE_DONE on success.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MESA_DRI_PATH
#define MESA_DRI_PATH "/run/opengl-driver/lib/dri"
#endif
#ifndef MESA_EGL_VENDOR
#define MESA_EGL_VENDOR "/run/opengl-driver/share/glvnd/egl_vendor.d/50_mesa.json"
#endif

int main(void)
{
    // Force Mesa to the virgl driver + surfaceless platform + our driver/vendor
    // paths. setenv before any EGL call so the loader sees them.
    setenv("EGL_PLATFORM", "surfaceless", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "virtio_gpu", 1);
    setenv("LIBGL_DRIVERS_PATH", MESA_DRI_PATH, 1);
    setenv("__EGL_VENDOR_LIBRARY_FILENAMES", MESA_EGL_VENDOR, 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "0", 1);
    setenv("MESA_DEBUG", "1", 1);
    setenv("EGL_LOG_LEVEL", "debug", 1);
    // Avoid Mesa trying to load a system drirc / shader cache it can't find.
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);

    fprintf(stderr, "PROBE_START dri=%s vendor=%s\n", MESA_DRI_PATH, MESA_EGL_VENDOR);

    EGLDisplay dpy = EGL_NO_DISPLAY;
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDisp =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getPlatDisp)
        dpy = getPlatDisp(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    fprintf(stderr, "PROBE_DISPLAY %p\n", (void*)dpy);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "PROBE_FAIL no display\n"); return 1; }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "PROBE_FAIL eglInitialize err=0x%x\n", eglGetError());
        return 2;
    }
    fprintf(stderr, "PROBE_EGL %d.%d vendor=%s renderer-init-ok\n",
            major, minor, eglQueryString(dpy, EGL_VENDOR));

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "PROBE_FAIL chooseConfig err=0x%x ncfg=%d\n", eglGetError(), ncfg);
        return 3;
    }
    fprintf(stderr, "PROBE_CONFIG ok ncfg=%d\n", ncfg);

    const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "PROBE_FAIL createContext err=0x%x\n", eglGetError());
        return 4;
    }
    fprintf(stderr, "PROBE_CONTEXT ok\n");

    const EGLint pbAttr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttr);
    if (surf == EGL_NO_SURFACE) {
        fprintf(stderr, "PROBE_WARN pbuffer err=0x%x — trying surfaceless makeCurrent\n",
                eglGetError());
    }

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "PROBE_FAIL makeCurrent err=0x%x\n", eglGetError());
        return 5;
    }
    fprintf(stderr, "PROBE_CURRENT ok GL_VENDOR=%s GL_RENDERER=%s GL_VERSION=%s\n",
            (const char*)glGetString(GL_VENDOR),
            (const char*)glGetString(GL_RENDERER),
            (const char*)glGetString(GL_VERSION));

    // Real GL work — forces resource creation + a command submission (EXECBUFFER).
    glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    unsigned char px[4] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(stderr, "PROBE_PIXEL r=%u g=%u b=%u a=%u (glErr=0x%x)\n",
            px[0], px[1], px[2], px[3], glGetError());

    fprintf(stderr, "PROBE_DONE\n");
    return 0;
}
