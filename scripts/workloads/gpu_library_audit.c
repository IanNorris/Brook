// Diagnostic-only ELF loader audit; do not interpose dlopen or change lookup.
#define _GNU_SOURCE
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned int la_version(unsigned int version) {
    return version < LAV_CURRENT ? version : LAV_CURRENT;
}

unsigned int la_objopen(struct link_map *map, Lmid_t ns, uintptr_t *cookie) {
    (void)ns; (void)cookie;
    const char *path = map->l_name;
    if (path && (strstr(path, "libEGL") || strstr(path, "libGLX") ||
                 strstr(path, "libgallium") || strstr(path, "iris_dri"))) {
        const char *depth = getenv("BROOK_MESA_WAYLAND_BUFFERS");
        const char *vendor = getenv("__EGL_VENDOR_LIBRARY_DIRS");
        char line[2048];
        int n = snprintf(line, sizeof(line), "BROOK_GPU_AUDIT pid=%ld path=%s depth=%s vendor=%s\n",
                         (long)getpid(), path, depth ? depth : "unset", vendor ? vendor : "unset");
        if (n > 0) write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
    return 0;
}
