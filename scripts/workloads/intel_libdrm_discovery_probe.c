#include <glob.h>
#include <stdio.h>
#include <xf86drm.h>

int main(void)
{
    glob_t matches = {0};
    int result = glob("/dev/dri/renderD*", 0, NULL, &matches);
    printf("glob=%d count=%zu\n", result, matches.gl_pathc);
    for (size_t i = 0; i < matches.gl_pathc; ++i)
        printf("glob_node=%s\n", matches.gl_pathv[i]);
    globfree(&matches);

    drmDevicePtr devices[16] = {0};
    int count = drmGetDevices2(0, devices, 16);
    printf("drmGetDevices2=%d\n", count);
    if (count < 0) return 1;
    for (int i = 0; i < count; ++i) {
        printf("device=%d bus=%d nodes=0x%x", i, devices[i]->bustype,
               devices[i]->available_nodes);
        for (int j = 0; j < DRM_NODE_MAX; ++j)
            if (devices[i]->available_nodes & (1 << j))
                printf(" node%d=%s", j, devices[i]->nodes[j]);
        putchar('\n');
    }
    drmFreeDevices(devices, count);
    return 0;
}
