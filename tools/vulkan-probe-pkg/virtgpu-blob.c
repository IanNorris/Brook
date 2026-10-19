// Raw HOST3D blob lifetime/bounds regression, shared by Linux and Brook.
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <sys/eventfd.h>
#include <poll.h>
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
static int descriptor_count(void) {
    int count=0;
    for(int fd=0;fd<1024;++fd)if(fcntl(fd,F_GETFD)>=0)++count;
    return count;
}
static int fence_contract(int prime) {
    const unsigned before=descriptor_count();
    for(uint32_t flags=1;flags<=3;++flags) {
        struct dma_buf_export_sync_file out={.flags=flags,.fd=-1};
        CHECK(!ioctl(prime,DMA_BUF_IOCTL_EXPORT_SYNC_FILE,&out));CHECK(out.fd>=0);
        CHECK(fcntl(out.fd,F_GETFD)&FD_CLOEXEC);
        struct pollfd fence_ready={.fd=out.fd,.events=POLLIN};
        CHECK(poll(&fence_ready,1,10000)==1 && fence_ready.revents==POLLIN);
        struct sync_file_info info={0};CHECK(!ioctl(out.fd,SYNC_IOC_FILE_INFO,&info));CHECK(info.status==1);
        struct dma_buf_import_sync_file in={.flags=flags,.fd=out.fd};
        CHECK(!ioctl(prime,DMA_BUF_IOCTL_IMPORT_SYNC_FILE,&in));
        info=(struct sync_file_info){0}; // query again; the first ioctl filled num_fences
        CHECK(!ioctl(out.fd,SYNC_IOC_FILE_INFO,&info)); // import never consumes fd
        close(out.fd);
    }
    for(uint32_t flags=0;flags<=8;flags+=4) {
        struct dma_buf_export_sync_file out={.flags=flags,.fd=-1};
        errno=0;CHECK(ioctl(prime,DMA_BUF_IOCTL_EXPORT_SYNC_FILE,&out)==-1 && errno==EINVAL);
        struct dma_buf_import_sync_file in={.flags=flags,.fd=-1};
        errno=0;CHECK(ioctl(prime,DMA_BUF_IOCTL_IMPORT_SYNC_FILE,&in)==-1 && errno==EINVAL);
    }
    struct dma_buf_import_sync_file in={.flags=3,.fd=-1};
    errno=0;CHECK(ioctl(prime,DMA_BUF_IOCTL_IMPORT_SYNC_FILE,&in)==-1 && errno==EINVAL);
    int event=eventfd(1,EFD_CLOEXEC);CHECK(event>=0);in.fd=event;
    errno=0;CHECK(ioctl(prime,DMA_BUF_IOCTL_IMPORT_SYNC_FILE,&in)==-1 && errno==EINVAL);close(event);
    in.fd=0x7fffffff;errno=0;CHECK(ioctl(prime,DMA_BUF_IOCTL_IMPORT_SYNC_FILE,&in)==-1 && errno==EINVAL);
    void* page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(page!=MAP_FAILED);
    *(struct dma_buf_export_sync_file*)page=(struct dma_buf_export_sync_file){.flags=3,.fd=-1};
    CHECK(!mprotect(page,4096,PROT_READ));errno=0;
    CHECK(ioctl(prime,DMA_BUF_IOCTL_EXPORT_SYNC_FILE,page)==-1 && errno==EFAULT);
    CHECK(!munmap(page,4096));CHECK((unsigned)descriptor_count()==before);
    return 0;
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
    struct drm_virtgpu_execbuffer submit={.flags=VIRTGPU_EXECBUF_RING_IDX,.ring_idx=0,
        .num_bo_handles=1,.bo_handles=(uintptr_t)&handle,.fence_fd=-1};
    CHECK(!ioctl(source,DRM_IOCTL_VIRTGPU_EXECBUFFER,&submit)); // implicit reservation, no outgoing fd
    struct drm_virtgpu_execbuffer malformed=submit;malformed.bo_handles=1;errno=0;
    CHECK(ioctl(source,DRM_IOCTL_VIRTGPU_EXECBUFFER,&malformed)==-1 && errno==EFAULT);
    CHECK(!fence_contract(prime.fd));
    struct drm_prime_handle second={.handle=imported.handle,.flags=DRM_CLOEXEC|DRM_RDWR};
    CHECK(!ioctl(target,DRM_IOCTL_PRIME_HANDLE_TO_FD,&second));
    CHECK(!drop(source,handle));close(source);close(prime.fd);
    CHECK(!fence_contract(second.fd));
    for(unsigned i=0;i<8192;++i)CHECK(alias[i]==(uint8_t)(i*17+3));
    struct drm_virtgpu_map partial={.handle=imported.handle};
    CHECK(!ioctl(target,DRM_IOCTL_VIRTGPU_MAP,&partial));
    uint8_t* middle=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,target,partial.offset+4096);
    // DRM core resolves the issued object's exact start token, not an interior
    // page token. The reservation must still prevent aliasing a later object.
    CHECK(middle==MAP_FAILED&&errno==EINVAL);
    CHECK(!drop(target,imported.handle));close(target);
    CHECK(!fence_contract(second.fd));close(second.fd);
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
    printf("{\"blob_contract_passed\":true,\"large_extents\":%u,\"shared_fork_remap\":true,\"dma_buf_fences\":true}\n",count);
    return 0;
}
