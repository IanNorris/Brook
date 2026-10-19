// Exercise the real resource_map/unmap functions with instrumented backends.
#include "config.h"
#include "virglrenderer.h"
#include "virgl_resource.h"
#include "virgl_context.h"
#include "venus/vkr_allocator.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"map boundary line %d: %s\n",__LINE__,#x);abort();} } while(0)
static struct virgl_resource resource;
static struct virgl_context context;
static char storage[16384];
static uint64_t actual_size;
static unsigned maps,unmaps;
int vrend_renderer_resource_map(struct pipe_resource*,void**,uint64_t*);
int vrend_renderer_resource_unmap(struct pipe_resource*);
void* __wrap_mmap(void*,size_t,int,int,int,off_t);
void* __wrap_mmap64(void*,size_t,int,int,int,off_t);
int __wrap_munmap(void*,size_t);
struct virgl_resource* virgl_resource_lookup(uint32_t id) {return id==1?&resource:NULL;}
struct virgl_context* virgl_context_lookup(uint32_t id) {(void)id;return &context;}
enum virgl_resource_fd_type virgl_resource_export_fd(struct virgl_resource* r,int* fd) {(void)r;*fd=-1;return VIRGL_RESOURCE_FD_SHM;}
int vrend_renderer_resource_map(struct pipe_resource* r,void** map,uint64_t* size) {(void)r;++maps;*map=storage;*size=actual_size;return 0;}
int vrend_renderer_resource_unmap(struct pipe_resource* r) {(void)r;++unmaps;return 0;}
int vkr_allocator_resource_map(struct virgl_resource* r,void** map,uint64_t* size) {(void)r;++maps;*map=storage;*size=actual_size;return 0;}
int vkr_allocator_resource_unmap(struct virgl_resource* r) {CHECK(r->mapped==storage);++unmaps;return 0;}
void* __wrap_mmap(void* addr,size_t size,int prot,int flags,int fd,off_t offset) {
    (void)addr;(void)prot;(void)flags;(void)fd;(void)offset;CHECK(size==resource.map_size);++maps;return storage;
}
void* __wrap_mmap64(void* a,size_t s,int p,int f,int fd,off_t o) {return __wrap_mmap(a,s,p,f,fd,o);}
int __wrap_munmap(void* addr,size_t size) {CHECK(addr==storage&&size==resource.map_size);++unmaps;return 0;}
static void reset(int pipe,uint64_t declared,uint64_t actual) {
    memset(&resource,0,sizeof(resource));memset(&context,0,sizeof(context));
    resource.map_size=declared;resource.fd_type=VIRGL_RESOURCE_FD_OPAQUE;
    if(pipe)resource.pipe_resource=(struct pipe_resource*)(uintptr_t)1;
    actual_size=actual;maps=unmaps=0;
}
int main(void) {
    for(int pipe=0;pipe<2;++pipe)for(unsigned i=0;i<3;++i) {
        const uint64_t sizes[]={2048,4096,8192};reset(pipe,4096,sizes[i]);
        void* output=(void*)(uintptr_t)42;uint64_t bytes=99;
        int result=virgl_renderer_resource_map(1,&output,&bytes);
        if(sizes[i]==4096) {
            CHECK(!result&&output==storage&&bytes==4096&&resource.mapped==storage);
            CHECK(!virgl_renderer_resource_unmap(1));
        } else {
            CHECK(result==-EINVAL&&output==(void*)(uintptr_t)42&&bytes==99);
            CHECK(!resource.mapped&&resource.map_size==4096&&!resource.mapped_from_pipe_resource);
            // A rejected attempt must leave a valid subsequent mapping possible.
            actual_size=4096;CHECK(!virgl_renderer_resource_map(1,&output,&bytes));
            CHECK(output==storage&&bytes==4096);CHECK(!virgl_renderer_resource_unmap(1));
        }
        CHECK(maps==unmaps);
    }
    for(int pipe=0;pipe<2;++pipe) {
        reset(pipe,0,8192);void* output=NULL;uint64_t bytes=0;
        CHECK(!virgl_renderer_resource_map(1,&output,&bytes)&&bytes==8192);
        CHECK(!virgl_renderer_resource_unmap(1)&&maps==unmaps);
    }
    for(unsigned i=0;i<2;++i) {
        reset(0,4096,4096);resource.fd_type=i?VIRGL_RESOURCE_FD_SHM:VIRGL_RESOURCE_FD_DMABUF;
        void* output=NULL;uint64_t bytes=0;
        CHECK(!virgl_renderer_resource_map(1,&output,&bytes)&&bytes==4096);
        CHECK(!virgl_renderer_resource_unmap(1)&&maps==1&&unmaps==1);
    }
    puts("RENDERER_MAP_BOUNDARY_PASS real map/unmap, PIPE/opaque over/undersize rollback, retry, SHM/DMABUF, legacy");
    return 0;
}
