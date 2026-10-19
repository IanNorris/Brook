// Context ABI cases run identically in Linux reference and Brook.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <virtgpu_drm.h>

static int check(const char* name, struct drm_virtgpu_context_set_param* params,
                 uint32_t count, int expected, int repeat)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 0; }
    struct drm_virtgpu_context_init init = {.num_params=count,.ctx_set_params=(uintptr_t)params};
    int result = ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init);
    int error = result ? errno : 0;
    int passed = error == expected;
    int repeated = -1;
    if (repeat && !error) {
        result = ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init);
        repeated = result ? errno : 0;
        passed &= repeated == EEXIST;
    }
    close(fd);
    printf("{\"case\":\"%s\",\"errno\":%d,\"expected\":%d,\"repeat_errno\":%d,\"passed\":%s}\n",
           name,error,expected,repeated,passed?"true":"false");
    return passed;
}
int main(void)
{
    struct drm_virtgpu_context_set_param p[5] = {{1,1},{2,64},{3,0}};
    int passed = check("virgl-64-rings",p,3,0,1);
    passed &= check("default",NULL,0,0,1);
    p[0].value=2;passed &= check("virgl2-64-rings",p,3,0,1);
    p[0].value=63;passed &= check("unavailable-capset",p,3,EINVAL,0);
    p[0].value=64;passed &= check("capset-overflow",p,3,EINVAL,0);
    p[0].value=1;p[1].value=65;passed &= check("ring-overflow",p,3,EINVAL,0);
    p[1].value=1;p[2].value=2;passed &= check("poll-outside-rings",p,3,EINVAL,0);
    p[2].value=0;p[1].param=1;p[1].value=1;passed &= check("duplicate-capset",p,2,EINVAL,0);
    p[1].param=99;passed &= check("unknown-param",p,2,EINVAL,0);
    passed &= check("too-many-params",p,5,EINVAL,0);
    passed &= check("bad-param-pointer",(void*)(uintptr_t)1,1,EFAULT,0);
    printf("{\"context_contract_passed\":%s}\n",passed?"true":"false");
    return passed?0:1;
}
