// Raw HOST3D blob lifetime/bounds regression, shared by Linux and Brook.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <drm.h>
#include <virtgpu_drm.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"blob line %d errno=%d: %s\n",__LINE__,errno,#x);return 1; } } while(0)
static int context(void) {
    int fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);
    if(fd<0)return -1;
    struct drm_virtgpu_context_set_param p[]={{1,4},{2,64},{3,0}};
    struct drm_virtgpu_context_init init={.num_params=3,.ctx_set_params=(uintptr_t)p};
    if(ioctl(fd,DRM_IOCTL_VIRTGPU_CONTEXT_INIT,&init)) {close(fd);return -1;}
    return fd;
}
static uint32_t create(int fd,uint64_t size) {
    struct drm_virtgpu_resource_create_blob b={.blob_mem=2,.blob_flags=1,.size=size};
    if(ioctl(fd,DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB,&b))return 0;
    return b.bo_handle;
}
static int drop(int fd,uint32_t handle) {
    struct drm_gem_close close_handle={.handle=handle};
    return ioctl(fd,DRM_IOCTL_GEM_CLOSE,&close_handle);
}
static void* map(int fd,uint32_t handle,size_t length) {
    struct drm_virtgpu_map m={.handle=handle};
    if(ioctl(fd,DRM_IOCTL_VIRTGPU_MAP,&m))return MAP_FAILED;
    return mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,m.offset);
}
int main(void) {
    int source=context();CHECK(source>=0);
    uint32_t handle=create(source,8192);CHECK(handle);
    uint8_t* memory=map(source,handle,8192);CHECK(memory!=MAP_FAILED);
    for(unsigned i=0;i<8192;++i)memory[i]=(uint8_t)(i*17+3);
    errno=0;void* invalid=map(source,handle,12288);CHECK(invalid==MAP_FAILED&&errno==EINVAL);
    struct drm_prime_handle prime={.handle=handle,.flags=DRM_CLOEXEC|DRM_RDWR};
    CHECK(!ioctl(source,DRM_IOCTL_PRIME_HANDLE_TO_FD,&prime));
    int target=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);CHECK(target>=0);
    struct drm_prime_handle imported={.fd=prime.fd};
    CHECK(!ioctl(target,DRM_IOCTL_PRIME_FD_TO_HANDLE,&imported));
    uint8_t* alias=map(target,imported.handle,8192);CHECK(alias!=MAP_FAILED);
    CHECK(!drop(source,handle));close(source);close(prime.fd);
    for(unsigned i=0;i<8192;++i)CHECK(alias[i]==(uint8_t)(i*17+3));
    struct drm_virtgpu_map partial={.handle=imported.handle};
    CHECK(!ioctl(target,DRM_IOCTL_VIRTGPU_MAP,&partial));
    uint8_t* middle=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,target,partial.offset+4096);
    // DRM core resolves the issued object's exact start token, not an interior
    // page token. The reservation must still prevent aliasing a later object.
    CHECK(middle==MAP_FAILED&&errno==EINVAL);
    CHECK(!drop(target,imported.handle));close(target);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        if(mprotect(alias,8192,PROT_NONE)||mprotect(alias,8192,PROT_READ|PROT_WRITE))_exit(2);
        void* destination=mmap(NULL,8192,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(destination==MAP_FAILED)_exit(3);
        uint8_t* moved=mremap(alias,8192,8192,MREMAP_MAYMOVE|MREMAP_FIXED,destination);
        if(moved==MAP_FAILED||moved[123]!=(uint8_t)(123*17+3))_exit(4);
        moved[123]=0x5a;
        if(munmap(moved,8192)||munmap(memory,8192))_exit(5);
        _exit(0);
    }
    int status=0;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    CHECK(memory[123]==0x5a&&alias[123]==0x5a);
    CHECK(!munmap(memory,8192)&&!munmap(alias,8192));
    // Reserve aperture capacity without touching the lazily backed host pages.
    int fd=context();CHECK(fd>=0);uint32_t large[16]={0};unsigned count=0;
    for(;count<16;++count) {
        large[count]=create(fd,256u*1024u*1024u);
        if(!large[count])break;
    }
    CHECK(count>0&&count<16);
    for(unsigned i=0;i<count;++i)CHECK(!drop(fd,large[i]));
    // Brook retirement is deferred. Yield boundedly until its verified unmaps
    // make an extent available again; the Linux reference usually reuses now.
    uint32_t fresh=0;
    for(unsigned i=0;i<200&&!fresh;++i) {fresh=create(fd,4096);if(!fresh)usleep(10000);}
    CHECK(fresh);CHECK(!drop(fd,fresh));close(fd);
    printf("{\"blob_contract_passed\":true,\"large_extents\":%u,\"shared_fork_remap\":true}\n",count);
    return 0;
}
