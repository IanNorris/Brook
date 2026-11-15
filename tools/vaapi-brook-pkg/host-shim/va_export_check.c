// va_export_check.c: create surfaces the way Chromium's VaapiVideoDecoder
// does (no pixel-format attribute) and export one as DRM_PRIME_2, printing the
// descriptor. Exit 0 if the export succeeds with an NV12 two-plane layout.
// usage: va_export_check /dev/dri/renderDxxx [width height]
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int w = argc > 3 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 2; }
    VADisplay dpy = vaGetDisplayDRM(fd);
    int maj, min;
    if (vaInitialize(dpy, &maj, &min) != VA_STATUS_SUCCESS) { puts("VAEXPORT FAIL init"); return 1; }
    printf("vendor: %s\n", vaQueryVendorString(dpy));
    VASurfaceID s[4];
    VAStatus st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, w, h, s, 4, NULL, 0);
    if (st) { printf("VAEXPORT FAIL create %d\n", st); return 1; }
    VADRMPRIMESurfaceDescriptor d = {0};
    st = vaExportSurfaceHandle(dpy, s[0], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d);
    if (st) { printf("VAEXPORT FAIL export %d (%s)\n", st, vaErrorStr(st)); return 1; }
    printf("fourcc=%.4s %ux%u objects=%u layers=%u\n", (char *)&d.fourcc, d.width, d.height, d.num_objects, d.num_layers);
    for (unsigned i = 0; i < d.num_objects; i++)
        printf(" object %u fd=%d size=%u modifier=%#llx\n", i, d.objects[i].fd, d.objects[i].size,
               (unsigned long long)d.objects[i].drm_format_modifier);
    for (unsigned i = 0; i < d.num_layers; i++)
        printf(" layer %u format=%.4s planes=%u offset=%u pitch=%u\n", i, (char *)&d.layers[i].drm_format,
               d.layers[i].num_planes, d.layers[i].offset[0], d.layers[i].pitch[0]);
    vaDestroySurfaces(dpy, s, 4);
    vaTerminate(dpy);
    int ok = d.num_layers == 2 && d.num_objects == 1;
    printf("VAEXPORT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
