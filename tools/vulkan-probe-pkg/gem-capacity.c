// Owned low-memory VirGL handle-capacity and reuse control. Run in a VM only.
#define _GNU_SOURCE
#include <drm.h>
#include <virtgpu_drm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static uint32_t create(int fd) {
    struct drm_virtgpu_resource_create r={.target=0,.format=64,.bind=16,
        .width=4096,.height=1,.depth=1,.array_size=1,.size=4096,.stride=4096};
    return ioctl(fd,DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,&r)?0:r.bo_handle;
}
static int drop(int fd,uint32_t handle) {
    struct drm_gem_close c={.handle=handle};return ioctl(fd,DRM_IOCTL_GEM_CLOSE,&c);
}
int main(int argc,char **argv) {
    const unsigned target=4096;int expect_limit=argc==2&&!strcmp(argv[1],"--expect-limit");
    if(argc>2||(argc==2&&!expect_limit))return 2;
    int fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);if(fd<0){perror("render node");return 2;}
    struct drm_virtgpu_context_set_param param={.param=VIRTGPU_CONTEXT_PARAM_CAPSET_ID,.value=1};
    struct drm_virtgpu_context_init init={.num_params=1,.ctx_set_params=(uintptr_t)&param};
    if(ioctl(fd,DRM_IOCTL_VIRTGPU_CONTEXT_INIT,&init)){perror("context init");close(fd);return 2;}
    uint32_t *handles=calloc(target,sizeof(*handles));if(!handles)return 2;
    unsigned count=0;int error=0,failed=0;
    for(;count<target;++count){handles[count]=create(fd);if(!handles[count]){error=errno;break;}}
    printf("created=%u target=%u errno=%d\n",count,target,error);fflush(stdout);
    if(expect_limit)failed=count!=2048||error!=ENOMEM;
    else failed=count!=target;
    if(count==target) {
        struct drm_virtgpu_map map={.handle=handles[3072]};
        if(ioctl(fd,DRM_IOCTL_VIRTGPU_MAP,&map))failed=1;
        else {
            unsigned char *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,map.offset);
            if(p==MAP_FAILED)failed=1;
            else {for(unsigned i=0;i<32;++i)p[i]=(unsigned char)(i^0x5a);
                for(unsigned i=0;i<32;++i)if(p[i]!=(unsigned char)(i^0x5a))failed=1;
                if(munmap(p,4096))failed=1;}
        }
    }
    // Close and reuse a real low slot after both success and expected exhaustion.
    if(count) {
        if(drop(fd,handles[0]))failed=1;
        handles[0]=create(fd);if(!handles[0])failed=1;
    }
    for(unsigned i=0;i<count;++i)if(handles[i]&&drop(fd,handles[i]))failed=1;
    if(close(fd))failed=1;
    free(handles);
    printf("GEMCAPACITY %s expected_limit=%d\n",failed?"FAIL":"PASS",expect_limit);return failed?1:0;
}
