/* Physical Intel fixture for separately imported layer buffers. */
#define _GNU_SOURCE
#include "renderer.h"
#include "surface.h"
#include "compositor_probe.h"
#include "desktop_tile.h"
#include "desktop_chrome_spans.h"
#include "gpu_output_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(c) do{if(!(c)){fprintf(stderr,"MULTI_COMPOSE_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct gem_close {uint32_t handle,pad;};
struct context {uint32_t id,pad;};
struct object {uint32_t handle,reloc_count;uint64_t relocs,alignment,offset,flags,reserved1,reserved2;};
struct exec {uint64_t objects;uint32_t count,start,length,dr1,dr4,clip_count;uint64_t clips,flags,context,fences;};
struct sync_object {uint32_t handle,flags;};
struct exec_fence {uint32_t handle,flags;};
struct sync_handle {uint32_t handle,flags;int32_t fd;uint32_t pad;};
struct domain {uint32_t handle,read,write;};
static int test_owner_exit(int drm,int protectedFd,struct context ctx,uint32_t batchHandle,uint32_t *cmd){
    struct prime imported={0,0,protectedFd};CHECK(!ioctl(drm,IOC(0x2e,sizeof(imported)),&imported) && imported.handle);
    struct sync_object sync={0,0};CHECK(!ioctl(drm,IOC(0xbf,sizeof(sync)),&sync) && sync.handle);
    int notification[2];CHECK(!pipe(notification));
    cmd[0]=0x10000002;cmd[1]=0x40011000;cmd[2]=0;cmd[3]=0xff13579b;cmd[4]=0x05000000;cmd[5]=0;
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        close(notification[0]);
        if(syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_ATTACH,0,0,0,0,0))_exit(10);
        long ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);
        if(ticket<=0)_exit(11);
        struct object objects[]={{imported.handle,0,0,4096,0x40011000,20,0,0},{batchHandle,0,0,4096,0x10000,16,0,0}};
        struct exec_fence signal={sync.handle,2};
        struct exec request={(uintptr_t)objects,2,0,24,0,0,1,(uintptr_t)&signal,(1u<<11)|(1u<<19),ctx.id,0};
        if(ioctl(drm,IOC(0x69,sizeof(request)),&request))_exit(12);
        if(write(notification[1],"S",1)!=1)_exit(13);
        for(;;)pause();
    }
    close(notification[1]);char ready=0;CHECK(read(notification[0],&ready,1)==1 && ready=='S');close(notification[0]);
    struct sync_handle export={sync.handle,1,-1,0};CHECK(!ioctl(drm,IOC(0xc1,sizeof(export)),&export) && export.fd>=0);
    struct pollfd pending={.fd=export.fd,.events=POLLIN};int before=poll(&pending,1,0);CHECK(before>=0);
    CHECK(!kill(child,SIGKILL));int status=0;CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    int attached=0;for(unsigned i=0;i<5000;++i){
        if(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_ATTACH,0,0,0,0,0)){attached=1;break;}
        CHECK(errno==EBUSY);usleep(1000);
    }CHECK(attached);
    long ticket=-1;for(unsigned i=0;i<5000;++i){
        ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);
        if(ticket>0)break;
        CHECK(errno==EBUSY);usleep(1000);
    }CHECK(ticket>0);
    CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0,0,0,0));
    CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_DETACH,0,0,0,0,0));
    pending.revents=0;CHECK(poll(&pending,1,0)==1 && (pending.revents&POLLIN));
    close(export.fd);CHECK(!ioctl(drm,IOC(0xc0,sizeof(sync)),&sync));
    struct gem_close closed={imported.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(closed)),&closed));
    fprintf(stderr,"OFFSCREEN_OWNER_EXIT_PASS fence_pending_before_kill=%d\n",before==0);
    return 0;
}
int main(int argc,char **argv){
    int ownerExit=argc==2 && !strcmp(argv[1],"--handoff-owner-exit");
    int handoff=ownerExit || (argc==2 && !strcmp(argv[1],"--handoff-output"));
    int protectedOutput=handoff || (argc==2 && !strcmp(argv[1],"--protected-output"));
    int gpuOutput=protectedOutput || (argc==2 && !strcmp(argv[1],"--gpu-output"));CHECK(argc==1 || gpuOutput);
    int drm=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);CHECK(drm>=0);
    struct create gem[2]={{65536,0,0},{65536,0,0}};
    struct create batch[2]={{4096,0,0},{4096,0,0}};uint32_t *cmd[2];
    struct context ctx={0};CHECK(!ioctl(drm,IOC(0x6d,sizeof(ctx)),&ctx) && ctx.id);
    struct prime prime[2];uint32_t *input[2];struct BrookVkExternalSource sources[2];
    for(unsigned i=0;i<2;++i){
        CHECK(!ioctl(drm,IOC(0x5b,sizeof(gem[i])),&gem[i]));
        struct map_offset map={gem[i].handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
        input[i]=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHECK(input[i]!=MAP_FAILED);
        prime[i]=(struct prime){gem[i].handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(prime[i])),&prime[i]));
        sources[i]=(struct BrookVkExternalSource){prime[i].fd,16384,-1};
        CHECK(!ioctl(drm,IOC(0x5b,sizeof(batch[i])),&batch[i]));
        struct map_offset bm={batch[i].handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(bm)),&bm));
        cmd[i]=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)bm.offset);CHECK(cmd[i]!=MAP_FAILED);
    }
    struct create dst={65536,0,0};struct prime dstPrime={0,0,-1};uint32_t *gpuPixels=MAP_FAILED;
    if(gpuOutput){
        CHECK(!ioctl(drm,IOC(0x5b,sizeof(dst)),&dst));
        struct map_offset dm={dst.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(dm)),&dm));
        gpuPixels=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)dm.offset);CHECK(gpuPixels!=MAP_FAILED);
        dstPrime=(struct prime){dst.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(dstPrime)),&dstPrime));
    }
    int targetFd=dstPrime.fd,protectedFd=-1;
    struct create tile={37*25*4,0,0};struct prime tilePrime={0,0,-1};uint32_t *tilePixels=MAP_FAILED;
    struct create crop={37*25*4,0,0};struct prime cropPrime={0,0,-1};uint32_t *cropPixels=MAP_FAILED;
    struct create packed={(3*37*25+10)*4,0,0};struct prime packedPrime={0,0,-1};uint32_t *packedPixels=MAP_FAILED;
    if(gpuOutput && !protectedOutput){
        CHECK(!ioctl(drm,IOC(0x5b,sizeof(tile)),&tile));
        struct map_offset tm={tile.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(tm)),&tm));
        tilePixels=mmap(NULL,tile.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)tm.offset);CHECK(tilePixels!=MAP_FAILED);
        tilePrime=(struct prime){tile.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(tilePrime)),&tilePrime));
        CHECK(!ioctl(drm,IOC(0x5b,sizeof(crop)),&crop));
        struct map_offset cm={crop.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(cm)),&cm));
        cropPixels=mmap(NULL,crop.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)cm.offset);CHECK(cropPixels!=MAP_FAILED);
        cropPrime=(struct prime){crop.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(cropPrime)),&cropPrime));
        CHECK(!ioctl(drm,IOC(0x5b,sizeof(packed)),&packed));
        struct map_offset pm={packed.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(pm)),&pm));
        packedPixels=mmap(NULL,packed.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)pm.offset);CHECK(packedPixels!=MAP_FAILED);
        for(uint64_t i=0;i<packed.size/4;++i)packedPixels[i]=0xcafebabe;
        packedPrime=(struct prime){packed.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(packedPrime)),&packedPrime));
    }
    if(protectedOutput){
        errno=0;CHECK(syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,0,0,0,0,0,0)==-1 && errno==EINVAL);
        errno=0;CHECK(syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,BROOK_GPU_OUTPUT_MAX_BYTES+1,0,0,0,0,0)==-1 && errno==EINVAL);
        errno=0;CHECK(syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,4096,1,0,0,0,0)==-1 && errno==EINVAL);
        protectedFd=(int)syscall(BROOK_SYS_GPU_OUTPUT_ALLOC,65536,0,0,0,0,0);CHECK(protectedFd>=0);
        CHECK(fcntl(protectedFd,F_GETFD)&FD_CLOEXEC);
        struct prime imported={0,0,protectedFd};CHECK(!ioctl(drm,IOC(0x2e,sizeof(imported)),&imported) && imported.handle);
        struct map_offset denied={imported.handle,0,0,2,0};CHECK(ioctl(drm,IOC(0x64,sizeof(denied)),&denied)==-1);
        struct gem_close closeImported={imported.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(closeImported)),&closeImported));
        targetFd=protectedFd;
    }
    BrookVkRenderer *r=gpuOutput?brook_vk_renderer_create_multi_output(16384,0):brook_vk_renderer_create_multi(16384,0);CHECK(r);
    BrookVkSurface *externalSurface=gpuOutput?NULL:brook_vk_surface_create(0);if(!gpuOutput)CHECK(externalSurface);
    if(ownerExit)CHECK(!test_owner_exit(drm,protectedFd,ctx,batch[0].handle,cmd[0]));
    if(handoff)CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_ATTACH,0,0,0,0,0));
    fprintf(stderr,"MULTI_COMPOSITOR_DEVICE %s\n",brook_vk_renderer_device(r));
    uint32_t scene[16384],reference[16384],out[4096];
    for(unsigned round=0;round<3;++round){
        compositor_probe_prepare(reference,16384,round);
        if(round)reference[21+16]|=BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE;
        memcpy(scene,reference,sizeof(scene));
        memcpy(input[0],reference,65536);memset(input[1],0,65536);memcpy(input[1],reference+4160,4096*4);
        scene[21]=0;memset(out,0,sizeof(out));
        struct sync_object sync[2]={{0,0},{0,0}};
        for(unsigned i=0;i<2;++i){
            CHECK(!ioctl(drm,IOC(0xbf,sizeof(sync[i])),&sync[i]) && sync[i].handle);
            uint32_t at=i?259:64,value=i?0x80402010u:0xff123456u;
            cmd[i][0]=0x10000002;cmd[i][1]=0x40011000+at*4;cmd[i][2]=0;cmd[i][3]=value;cmd[i][4]=0x05000000;cmd[i][5]=0;
            struct object objects[]={{gem[i].handle,0,0,4096,0x40011000,20,0,0},{batch[i].handle,0,0,4096,0x10000,16,0,0}};
            struct exec_fence ef={sync[i].handle,2};
            struct exec request={(uintptr_t)objects,2,0,24,0,0,1,(uintptr_t)&ef,(1u<<11)|(1u<<19),ctx.id,0};
            if(ioctl(drm,IOC(0x69,sizeof(request)),&request)){fprintf(stderr,"MULTI_PRODUCER_UNCERTAIN resources retained\n");for(;;)pause();}
            struct sync_handle handle={sync[i].handle,1,-1,0};
            if(ioctl(drm,IOC(0xc1,sizeof(handle)),&handle) || handle.fd<0){fprintf(stderr,"MULTI_FENCE_UNCERTAIN resources retained\n");for(;;)pause();}
            sources[i].syncFd=handle.fd;struct pollfd p={.fd=handle.fd,.events=POLLIN};int ready=poll(&p,1,0);
            fprintf(stderr,"MULTI_PRODUCER_FENCE layer=%u ready=%d revents=%d\n",i,ready,p.revents);
            reference[i?4160+at:at]=value;
        }
        long ticket=0;
        if(handoff){ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,targetFd,0,0,0,0);CHECK(ticket>0);}
        /* The separate-output path accepts metadata alone. */
        size_t sceneWords=gpuOutput?4+2*17:16384;
        if(gpuOutput)scene[3]=0;
        BrookVkResult status=gpuOutput?brook_vk_renderer_compose_multi_output(r,scene,sceneWords,sources,2,targetFd,16384,10000000000ULL):
            brook_vk_renderer_compose_multi(r,scene,16384,sources,2,out,4096,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"MULTI_COMPOSE_UNCERTAIN resources retained\n");for(;;)pause();}
        if(status!=BROOK_VK_COMPLETE){fprintf(stderr,"MULTI_PRODUCER_MAY_BE_ACTIVE resources retained\n");for(;;)pause();}
        if(externalSurface){
            struct BrookVkSurfaceExternalLayer layers[]={
                {sources[0].fd,sources[0].syncFd,sources[0].words,64,64,64,64,0,0,64,64,0,0,64,64,0,0,64,64,255,0},
                {sources[1].fd,sources[1].syncFd,sources[1].words,0,64,64,64,3,4,23,19,-4,7,46,38,2,10,38,24,
                 reference[21+16]&255,reference[21+16]&~255u}
            };
            uint32_t direct[4096];
            status=brook_vk_surface_compose_external(externalSurface,64,64,layers,2,direct,4096);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"SURFACE_EXTERNAL_UNCERTAIN resources retained\n");for(;;)pause();}
            CHECK(status==BROOK_VK_COMPLETE);
            for(unsigned i=0;i<4096;++i)CHECK(direct[i]==compositor_probe_pixel(reference,i%64,i/64));
            layers[1].stride=1;direct[0]=0x12345678;
            CHECK(brook_vk_surface_compose_external(externalSurface,64,64,layers,2,direct,4096)==BROOK_VK_REJECTED && direct[0]==0x12345678);
        }
        if(handoff){
            long complete=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0,0,0,0);
            fprintf(stderr,"OFFSCREEN_HANDOFF round=%u ticket=%ld state=%ld errno=%d\n",round,ticket,complete,errno);
            CHECK(complete==1);
            CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0,0,0,0));
        }
        for(unsigned i=0;i<2;++i){close(sources[i].syncFd);sources[i].syncFd=-1;CHECK(!ioctl(drm,IOC(0xc0,sizeof(sync[i])),&sync[i]));}
        if(protectedOutput){
            uint32_t pass[16384]={0};pass[0]=64;pass[1]=64;pass[2]=1;pass[3]=0;
            const uint32_t layer[]={0,64,64,64,0,0,64,64,0,0,64,64,0,0,64,64,255};memcpy(pass+4,layer,sizeof(layer));
            const struct BrookVkExternalSource protectedSource={protectedFd,16384,-1};
            status=brook_vk_renderer_compose_multi_output(r,pass,16384,&protectedSource,1,dstPrime.fd,16384,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"PROTECTED_COPY_UNCERTAIN resources retained\n");for(;;)pause();}
            CHECK(status==BROOK_VK_COMPLETE);
        }
        if(gpuOutput){struct domain d={dst.handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));memcpy(out,gpuPixels,sizeof(out));}
        for(unsigned i=0;i<4096;++i){uint32_t want=compositor_probe_pixel(reference,i%64,i/64);
            if(out[i]!=want){fprintf(stderr,"mismatch round=%u pixel=%u got=%08x expected=%08x\n",round,i,out[i],want);return 1;}}
        if(gpuOutput && !protectedOutput){
            uint32_t tileScene[4+2*17];size_t sourceWords[]={sources[0].words,sources[1].words};
            CHECK(desktop_tile_scene(scene,4+2*17,sourceWords,2,7,9,37,25,tileScene,4+2*17));
            CHECK(!desktop_tile_scene(scene,4+2*17,sourceWords,2,60,9,37,25,tileScene,4+2*17));
            status=brook_vk_renderer_compose_multi_output(r,tileScene,4+2*17,sources,2,tilePrime.fd,tile.size/4,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"TILE_COMPOSE_UNCERTAIN resources retained\n");for(;;)pause();}
            CHECK(status==BROOK_VK_COMPLETE);
            struct domain d={tile.handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));
            for(unsigned y=0;y<25;++y)for(unsigned x=0;x<37;++x){
                uint32_t want=compositor_probe_pixel(reference,x+7,y+9);
                if(tilePixels[y*37+x]!=want){fprintf(stderr,"tile mismatch round=%u x=%u y=%u got=%08x expected=%08x\n",round,x,y,tilePixels[y*37+x],want);return 1;}
            }
            struct domain sourceDomain={gem[0].handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(sourceDomain)),&sourceDomain));
            CHECK(desktop_tile_crop_backdrop(scene,38,sourceWords,2,7,9,37,25,input[0],cropPixels,crop.size/4,tileScene,38));
            struct BrookVkExternalSource croppedSources[]={ {cropPrime.fd,crop.size/4,-1},sources[1] };
            status=brook_vk_renderer_compose_multi_output(r,tileScene,38,croppedSources,2,tilePrime.fd,tile.size/4,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"CROPPED_TILE_UNCERTAIN resources retained\n");for(;;)pause();}
            CHECK(status==BROOK_VK_COMPLETE);CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));
            for(unsigned y=0;y<25;++y)for(unsigned x=0;x<37;++x){
                uint32_t want=compositor_probe_pixel(reference,x+7,y+9);
                if(tilePixels[y*37+x]!=want){fprintf(stderr,"cropped tile mismatch round=%u x=%u y=%u got=%08x expected=%08x\n",round,x,y,tilePixels[y*37+x],want);return 1;}
            }
            tileScene[3]=UINT32_MAX;
            CHECK(brook_vk_renderer_compose_multi_output(r,tileScene,38,croppedSources,2,packedPrime.fd,packed.size/4,10000000000ULL)==BROOK_VK_REJECTED);
            tileScene[3]=5+round*37*25;
            status=brook_vk_renderer_compose_multi_output(r,tileScene,38,croppedSources,2,packedPrime.fd,packed.size/4,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"PACKED_TILE_UNCERTAIN resources retained\n");for(;;)pause();}
            CHECK(status==BROOK_VK_COMPLETE);
        }
    }
    if(externalSurface){CHECK(brook_vk_surface_destroy(externalSurface));puts("VULKAN_EXTERNAL_SURFACE_PASS");}
    if(gpuOutput && !protectedOutput){
        struct domain d={packed.handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));
        CHECK(packedPixels[0]==0xcafebabe && packedPixels[4]==0xcafebabe && packedPixels[5+3*37*25]==0xcafebabe);
        for(unsigned round=0;round<3;++round){
            compositor_probe_prepare(reference,16384,round);
            if(round)reference[21+16]|=BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE;
            reference[64]=0xff123456;reference[4160+259]=0x80402010;
            for(unsigned y=0;y<25;++y)for(unsigned x=0;x<37;++x){
                uint32_t want=compositor_probe_pixel(reference,x+7,y+9),got=packedPixels[5+round*37*25+y*37+x];
                if(got!=want){fprintf(stderr,"packed tile mismatch round=%u x=%u y=%u got=%08x expected=%08x\n",round,x,y,got,want);return 1;}
            }
        }
        fprintf(stderr,"DESKTOP_PACKED_TILE_PASS rounds=3 size=37x25 cropped_backdrop=1\n");
        /* Exercise the batch shader used by partial desktop presentation with
         * the last scene's opaque-source flag and varying source alpha. */
        BrookVkRenderer *batch=brook_vk_renderer_create_batch_output(1,0);CHECK(batch);
        uint32_t tileScene[BROOK_VK_TILE_STRIDE]={0};size_t sourceWords[]={sources[0].words,sources[1].words};
        CHECK(desktop_tile_scene(scene,38,sourceWords,2,7,9,37,25,tileScene,BROOK_VK_TILE_STRIDE));
        tileScene[3]=0;
        BrookVkResult status=brook_vk_renderer_compose_batch_output(batch,tileScene,BROOK_VK_TILE_STRIDE,1,
                                     sources,2,tilePrime.fd,tile.size/4,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"OPAQUE_BATCH_UNCERTAIN resources retained\n");for(;;)pause();}
        CHECK(status==BROOK_VK_COMPLETE);
        struct domain tileDomain={tile.handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(tileDomain)),&tileDomain));
        for(unsigned y=0;y<25;++y)for(unsigned x=0;x<37;++x){
            uint32_t want=compositor_probe_pixel(reference,x+7,y+9),got=tilePixels[y*37+x];
            if(got!=want){fprintf(stderr,"opaque batch mismatch x=%u y=%u got=%08x expected=%08x\n",x,y,got,want);return 1;}
        }
        {
            // Opaque middle layer with a clipped translucent layer above it:
            // skipping lower layers must not alter blending or exposed edges.
            uint32_t stacked[55]={0};memcpy(stacked,scene,38*sizeof(uint32_t));stacked[2]=3;
            const uint32_t upper[]={0,64,64,64,0,0,32,32,12,8,32,32,0,0,64,64,127|BROOK_VK_STRAIGHT_ALPHA};
            memcpy(stacked+38,upper,sizeof(upper));
            struct create upperGem={65536,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(upperGem)),&upperGem));
            struct map_offset upperMap={upperGem.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(upperMap)),&upperMap));
            uint32_t *upperPixels=mmap(NULL,upperGem.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)upperMap.offset);
            CHECK(upperPixels!=MAP_FAILED);memcpy(upperPixels,reference+4160,4096*sizeof(uint32_t));
            struct prime upperPrime={upperGem.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(upperPrime)),&upperPrime));
            struct BrookVkExternalSource stackedSources[]={sources[0],sources[1],{upperPrime.fd,upperGem.size/4,-1}};
            size_t stackedWords[]={sourceWords[0],sourceWords[1],upperGem.size/4};
            CHECK(desktop_tile_scene(stacked,55,stackedWords,3,7,9,37,25,tileScene,BROOK_VK_TILE_STRIDE));tileScene[3]=0;
            status=brook_vk_renderer_compose_batch_output(batch,tileScene,BROOK_VK_TILE_STRIDE,1,
                                                          stackedSources,3,tilePrime.fd,tile.size/4,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"STACKED_BATCH_UNCERTAIN resources retained\n");for(;;)pause();}
            CHECK(status==BROOK_VK_COMPLETE && !ioctl(drm,IOC(0x5f,sizeof(tileDomain)),&tileDomain));
            for(unsigned y=0;y<25;++y)for(unsigned x=0;x<37;++x){
                const unsigned px=x+7,py=y+9;uint32_t want=compositor_probe_pixel(reference,px,py);
                if(px>=12 && px<44 && py>=8 && py<40){
                    const uint32_t src=reference[4160+(py-8)*64+(px-12)];
                    const uint32_t alpha=compositor_probe_scale(src>>24,127),inv=255-alpha;
                    uint32_t blended=0xff000000u;
                    for(unsigned c=0;c<3;++c){const uint32_t s=(src>>(8*c))&255u,d=(want>>(8*c))&255u;
                        blended|=((s*alpha+d*inv+127u)/255u)<<(8*c);}
                    want=blended;
                }
                CHECK(tilePixels[y*37+x]==want);
            }
            CHECK(!close(upperPrime.fd) && !munmap(upperPixels,upperGem.size));
            struct gem_close upperClose={upperGem.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(upperClose)),&upperClose));
            fprintf(stderr,"STACKED_OPAQUE_TRANSLUCENT_BATCH_PASS\n");
        }
        /* The second layer cannot intersect this tile. Submit only the
         * selected descriptor and compare against the independent full scene. */
        struct DesktopDamageSet outside={0};desktop_damage_add(&outside,(struct DesktopDamageRect){50,50,60,60},64,64);
        uint32_t filtered[38],selected[2];size_t kept=0;
        CHECK(desktop_tile_cull(scene,38,sourceWords,2,&outside,filtered,38,selected,&kept) && kept==1 && selected[0]==0);
        CHECK(desktop_tile_scene(filtered,38,sourceWords,1,50,50,10,10,tileScene,BROOK_VK_TILE_STRIDE));tileScene[3]=0;
        status=brook_vk_renderer_compose_batch_output(batch,tileScene,BROOK_VK_TILE_STRIDE,1,sources,1,tilePrime.fd,tile.size/4,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"CULLED_BATCH_UNCERTAIN resources retained\n");for(;;)pause();}
        CHECK(status==BROOK_VK_COMPLETE && !ioctl(drm,IOC(0x5f,sizeof(tileDomain)),&tileDomain));
        for(unsigned y=0;y<10;++y)for(unsigned x=0;x<10;++x)CHECK(tilePixels[y*10+x]==compositor_probe_pixel(reference,x+50,y+50));
        const uint32_t savedMode=scene[21+16],savedReferenceMode=reference[21+16];
        scene[21+16]&=~255u;reference[21+16]&=~255u;
        struct DesktopDamageSet transparent={0};desktop_damage_add(&transparent,(struct DesktopDamageRect){10,10,20,20},64,64);
        CHECK(desktop_tile_cull(scene,38,sourceWords,2,&transparent,filtered,38,selected,&kept) && kept==1);
        CHECK(desktop_tile_scene(filtered,38,sourceWords,1,10,10,10,10,tileScene,BROOK_VK_TILE_STRIDE));tileScene[3]=0;
        status=brook_vk_renderer_compose_batch_output(batch,tileScene,BROOK_VK_TILE_STRIDE,1,sources,1,tilePrime.fd,tile.size/4,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"CULLED_BATCH_UNCERTAIN resources retained\n");for(;;)pause();}
        CHECK(status==BROOK_VK_COMPLETE && !ioctl(drm,IOC(0x5f,sizeof(tileDomain)),&tileDomain));
        for(unsigned y=0;y<10;++y)for(unsigned x=0;x<10;++x)CHECK(tilePixels[y*10+x]==compositor_probe_pixel(reference,x+10,y+10));
        scene[21+16]=savedMode;reference[21+16]=savedReferenceMode;
        /* Drop a middle source and submit the later source through the
         * compacted descriptor list, as the desktop renderer does. */
        uint32_t three[55],compact[55],selection[3];size_t compactCount=0;
        memcpy(three,scene,38*sizeof(uint32_t));memcpy(three+38,scene+21,17*sizeof(uint32_t));
        three[2]=3;three[21+16]&=~255u;
        struct BrookVkExternalSource originals[]={sources[0],sources[0],sources[1]},chosen[3];
        size_t originalWords[]={sources[0].words,sources[0].words,sources[1].words},chosenWords[3];
        CHECK(desktop_tile_cull(three,55,originalWords,3,&transparent,compact,55,selection,&compactCount));
        CHECK(compactCount==2 && selection[0]==0 && selection[1]==2);
        for(size_t i=0;i<compactCount;++i){chosen[i]=originals[selection[i]];chosenWords[i]=originalWords[selection[i]];}
        CHECK(desktop_tile_scene(compact,55,chosenWords,compactCount,10,10,10,10,tileScene,BROOK_VK_TILE_STRIDE));tileScene[3]=0;
        status=brook_vk_renderer_compose_batch_output(batch,tileScene,BROOK_VK_TILE_STRIDE,1,chosen,compactCount,tilePrime.fd,tile.size/4,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"COMPACTED_BATCH_UNCERTAIN resources retained\n");for(;;)pause();}
        CHECK(status==BROOK_VK_COMPLETE && !ioctl(drm,IOC(0x5f,sizeof(tileDomain)),&tileDomain));
        for(unsigned y=0;y<10;++y)for(unsigned x=0;x<10;++x)CHECK(tilePixels[y*10+x]==compositor_probe_pixel(reference,x+10,y+10));
        fprintf(stderr,"COMPACTED_BATCH_DESCRIPTOR_PASS\n");
        {
            /* Render four independently imported chrome strips over the
             * backdrop and compare against a full-surface CPU oracle. */
            struct BrookDesktopLayer outer={.window=7,.incarnation=9,.width=32,.height=22,.clientX=4,.clientY=5};
            struct BrookDesktopLayer inner={.window=7,.incarnation=9,.clientX=9,.clientY=9,.clientWidth=22,.clientHeight=14};
            struct DesktopChromeSpan spans[4];CHECK(desktop_chrome_spans(&inner,&outer,spans)==4);
            uint32_t chromePixels[32*22];
            for(unsigned y=0;y<22;++y)for(unsigned x=0;x<32;++x)
                chromePixels[y*32+x]=(x>=5 && x<27 && y>=4 && y<18)?0:0xff2468acu;
            struct create parts[4];struct prime partFds[4];uint32_t *partPixels[4];
            struct BrookVkExternalSource stripSources[5]={sources[0]};size_t stripWords[5]={sources[0].words};
            uint32_t stripScene[4+5*17]={0};memcpy(stripScene,scene,21*sizeof(uint32_t));stripScene[2]=5;
            for(unsigned i=0;i<4;++i){const struct DesktopChromeSpan *s=&spans[i];
                parts[i]=(struct create){(uint64_t)s->width*s->height*4,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(parts[i])),&parts[i]));
                struct map_offset map={parts[i].handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(map)),&map));
                partPixels[i]=mmap(NULL,parts[i].size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)map.offset);CHECK(partPixels[i]!=MAP_FAILED);
                CHECK(desktop_chrome_copy_span(chromePixels,32,32,22,s,partPixels[i],parts[i].size/4));
                partFds[i]=(struct prime){parts[i].handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(partFds[i])),&partFds[i]));
                stripSources[i+1]=(struct BrookVkExternalSource){partFds[i].fd,parts[i].size/4,-1};stripWords[i+1]=parts[i].size/4;
                struct BrookVkSceneLayer l={0,s->width,s->width,s->height,0,0,s->width,s->height,
                    (int32_t)(outer.clientX+(int32_t)s->x),(int32_t)(outer.clientY+(int32_t)s->y),s->width,s->height,
                    0,0,64,64,255|BROOK_VK_STRAIGHT_ALPHA};
                memcpy(stripScene+4+(i+1)*17,&l,sizeof(l));
            }
            CHECK(desktop_tile_scene(stripScene,4+5*17,stripWords,5,7,9,37,25,tileScene,BROOK_VK_TILE_STRIDE));
            status=brook_vk_renderer_compose_batch_output(batch,tileScene,BROOK_VK_TILE_STRIDE,1,stripSources,5,tilePrime.fd,tile.size/4,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"CHROME_SPANS_UNCERTAIN resources retained\n");for(;;)pause();}
            CHECK(status==BROOK_VK_COMPLETE && !ioctl(drm,IOC(0x5f,sizeof(tileDomain)),&tileDomain));
            for(unsigned y=0;y<25;++y)for(unsigned x=0;x<37;++x){
                unsigned px=x+7,py=y+9;uint32_t expected=reference[64+py*64+px];
                if(px>=4 && px<36 && py>=5 && py<27){unsigned sx=px-4,sy=py-5;
                    if(!(sx>=5 && sx<27 && sy>=4 && sy<18))expected=0xff2468acu;}
                CHECK(tilePixels[y*37+x]==expected);
            }
            for(unsigned i=0;i<4;++i){CHECK(!munmap(partPixels[i],parts[i].size));CHECK(!close(partFds[i].fd));
                struct gem_close c={parts[i].handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
            fprintf(stderr,"CHROME_SPANS_GPU_PASS strips=4\n");
        }
        fprintf(stderr,"CULLED_BATCH_TILE_PASS offscreen=1 transparent=1\n");
        CHECK(brook_vk_renderer_destroy(batch));
        fprintf(stderr,"OPAQUE_BATCH_TILE_PASS size=37x25\n");
    }
    if(handoff){
        BrookVkRenderer *batch=brook_vk_renderer_create_batch_output(3,0);CHECK(batch);
        uint32_t scenes[3*BROOK_VK_TILE_STRIDE]={0};size_t sourceWords[]={sources[0].words,sources[1].words};
        const uint32_t xs[]={0,22,44},ys[]={0,14,32},tileWidth=20,tileHeight=16;
        struct create batchBackdrop={3*20*16*4,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(batchBackdrop)),&batchBackdrop));
        struct map_offset bm={batchBackdrop.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(bm)),&bm));
        uint32_t *backdropTiles=mmap(NULL,batchBackdrop.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)bm.offset);CHECK(backdropTiles!=MAP_FAILED);
        struct prime backdropPrime={batchBackdrop.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(backdropPrime)),&backdropPrime));
        struct domain sourceDomain={gem[0].handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(sourceDomain)),&sourceDomain));
        struct DesktopDamageSet damage={0};struct DesktopPackedTile tiles[3];
        for(unsigned i=0;i<3;++i)desktop_damage_add(&damage,(struct DesktopDamageRect){xs[i],ys[i],xs[i]+tileWidth,ys[i]+tileHeight},64,64);
        CHECK(damage.count==3 && desktop_tile_pack(scene,38,sourceWords,2,&damage,input[0],backdropTiles,batchBackdrop.size/4,
                                                    scenes,3*BROOK_VK_TILE_STRIDE,tiles,3));
        struct BrookVkExternalSource batchSources[]={ {backdropPrime.fd,batchBackdrop.size/4,-1},sources[1] };
        scenes[BROOK_VK_TILE_STRIDE+3]=0;
        CHECK(brook_vk_renderer_compose_batch_output(batch,scenes,3*BROOK_VK_TILE_STRIDE,3,batchSources,2,protectedFd,16384,10000000000ULL)==BROOK_VK_REJECTED);
        scenes[BROOK_VK_TILE_STRIDE+3]=(uint32_t)(tiles[1].offsetBytes/4);
        long ticket=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(ticket>0);
        BrookVkResult status=brook_vk_renderer_compose_batch_output(batch,scenes,3*BROOK_VK_TILE_STRIDE,3,batchSources,2,protectedFd,16384,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"BATCH_OUTPUT_UNCERTAIN resources retained\n");for(;;)pause();}
        CHECK(status==BROOK_VK_COMPLETE && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,ticket,0,0,0,0)==1);
        CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,ticket,0,0,0,0));
        uint32_t pass[38]={64,64,1,0};const uint32_t layer[]={0,64,64,64,0,0,64,64,0,0,64,64,0,0,64,64,255};memcpy(pass+4,layer,sizeof(layer));
        const struct BrookVkExternalSource protectedSource={protectedFd,16384,-1};
        status=brook_vk_renderer_compose_multi_output(r,pass,21,&protectedSource,1,dstPrime.fd,16384,10000000000ULL);
        if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"BATCH_READBACK_UNCERTAIN resources retained\n");for(;;)pause();}
        CHECK(status==BROOK_VK_COMPLETE);struct domain d={dst.handle,1,0};CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));
        for(unsigned i=0;i<3;++i)for(unsigned y=0;y<tileHeight;++y)for(unsigned x=0;x<tileWidth;++x){
            const uint32_t want=compositor_probe_pixel(reference,x+tiles[i].x,y+tiles[i].y),got=gpuPixels[tiles[i].offsetBytes/4+y*tileWidth+x];
            if(got!=want){fprintf(stderr,"batch mismatch tile=%u x=%u y=%u got=%08x expected=%08x\n",i,x,y,got,want);return 1;}
        }
        // Reconstruct successive changed scenes from the previously verified
        // full GPU output plus newly rendered damage, then compare every
        // screen pixel to the independent full-scene CPU oracle.
        uint32_t retained[4096];memcpy(retained,out,sizeof(retained));
        struct DesktopDamageState damageState={0};struct DesktopDamageSet footprint={0};
        desktop_damage_add(&footprint,(struct DesktopDamageRect){2,10,40,34},64,64);
        desktop_damage_commit(&damageState,1,1,64,64,footprint);
        for(unsigned frame=0;frame<3;++frame){
            const uint32_t opacity=frame==0?0:frame==1?127:255;
            scene[21+16]=reference[21+16]=opacity;
            struct DesktopDamageSet changed=desktop_damage_current(damageState,1,1,64,64,footprint);
            CHECK(changed.count==1);
            struct domain backdropCpu={batchBackdrop.handle,1,1};CHECK(!ioctl(drm,IOC(0x5f,sizeof(backdropCpu)),&backdropCpu));
            CHECK(desktop_tile_pack(scene,38,sourceWords,2,&changed,input[0],backdropTiles,batchBackdrop.size/4,
                                    scenes,3*BROOK_VK_TILE_STRIDE,tiles,3));
            long next=syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RESERVE,protectedFd,0,0,0,0);CHECK(next>0);
            status=brook_vk_renderer_compose_batch_output(batch,scenes,BROOK_VK_TILE_STRIDE,1,batchSources,2,protectedFd,16384,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"SEQUENCE_BATCH_UNCERTAIN resources retained\n");for(;;)pause();}
            CHECK(status==BROOK_VK_COMPLETE && syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_OBSERVE,next,0,0,0,0)==1);
            CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_RELEASE,next,0,0,0,0));
            status=brook_vk_renderer_compose_multi_output(r,pass,21,&protectedSource,1,dstPrime.fd,16384,10000000000ULL);
            if(status==BROOK_VK_UNCERTAIN){fprintf(stderr,"SEQUENCE_READBACK_UNCERTAIN resources retained\n");for(;;)pause();}
            CHECK(status==BROOK_VK_COMPLETE && !ioctl(drm,IOC(0x5f,sizeof(d)),&d));
            const struct DesktopPackedTile *tile=&tiles[0];
            for(unsigned y=0;y<tile->height;++y)for(unsigned x=0;x<tile->width;++x)
                retained[(tile->y+y)*64+tile->x+x]=gpuPixels[tile->offsetBytes/4+y*tile->width+x];
            for(unsigned i=0;i<4096;++i)CHECK(retained[i]==compositor_probe_pixel(reference,i%64,i/64));
            desktop_damage_commit(&damageState,1,1,64,64,footprint);
        }
        fprintf(stderr,"PROTECTED_DAMAGE_SEQUENCE_PASS frames=3\n");
        CHECK(brook_vk_renderer_destroy(batch));close(backdropPrime.fd);munmap(backdropTiles,batchBackdrop.size);
        struct gem_close bc={batchBackdrop.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(bc)),&bc));
        fprintf(stderr,"PROTECTED_BATCH_TILE_PASS tiles=3 single_submission=1 cropped_backdrop=1\n");
    }
    out[0]=0x12345678;
    if(gpuOutput){
        CHECK(brook_vk_renderer_compose_multi_output(r,scene,4+2*17-1,sources,2,targetFd,16384,10000000000ULL)==BROOK_VK_REJECTED);
        CHECK(brook_vk_renderer_compose_multi_output(r,scene,4+2*17,sources,2,targetFd,4095,10000000000ULL)==BROOK_VK_REJECTED);
        CHECK(brook_vk_renderer_compose_multi_output(r,scene,16384,sources,2,sources[0].fd,16384,10000000000ULL)==BROOK_VK_REJECTED);
        int alias=dup(sources[0].fd);CHECK(alias>=0);
        CHECK(brook_vk_renderer_compose_multi_output(r,scene,16384,sources,2,alias,16384,10000000000ULL)==BROOK_VK_REJECTED);close(alias);
    }
    int savedFd=sources[1].fd;sources[1].fd=sources[0].fd;
    CHECK((gpuOutput?brook_vk_renderer_compose_multi_output(r,scene,16384,sources,2,targetFd,16384,10000000000ULL):
           brook_vk_renderer_compose_multi(r,scene,16384,sources,2,out,4096,10000000000ULL))==BROOK_VK_REJECTED && out[0]==0x12345678);
    int alias=dup(sources[0].fd);CHECK(alias>=0);sources[1].fd=alias;
    CHECK((gpuOutput?brook_vk_renderer_compose_multi_output(r,scene,16384,sources,2,targetFd,16384,10000000000ULL):
           brook_vk_renderer_compose_multi(r,scene,16384,sources,2,out,4096,10000000000ULL))==BROOK_VK_REJECTED && out[0]==0x12345678);close(alias);
    sources[1].fd=savedFd;scene[21]=16380;
    CHECK((gpuOutput?brook_vk_renderer_compose_multi_output(r,scene,16384,sources,2,targetFd,16384,10000000000ULL):
           brook_vk_renderer_compose_multi(r,scene,16384,sources,2,out,4096,10000000000ULL))==BROOK_VK_REJECTED && out[0]==0x12345678);
    CHECK(brook_vk_renderer_destroy(r));
    if(handoff)CHECK(!syscall(BROOK_SYS_GPU_HANDOFF_CTL,BROOK_GPU_HANDOFF_DETACH,0,0,0,0,0));
    for(unsigned i=0;i<2;++i){close(prime[i].fd);munmap(input[i],65536);munmap(cmd[i],4096);
        struct gem_close bc={batch[i].handle,0},c={gem[i].handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(bc)),&bc));CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    CHECK(!ioctl(drm,IOC(0x6e,sizeof(ctx)),&ctx));
    if(protectedFd>=0)close(protectedFd);
    if(tilePrime.fd>=0){close(tilePrime.fd);munmap(tilePixels,tile.size);struct gem_close c={tile.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    if(cropPrime.fd>=0){close(cropPrime.fd);munmap(cropPixels,crop.size);struct gem_close c={crop.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    if(packedPrime.fd>=0){close(packedPrime.fd);munmap(packedPixels,packed.size);struct gem_close c={packed.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    if(gpuOutput){close(dstPrime.fd);munmap(gpuPixels,65536);struct gem_close c={dst.handle,0};CHECK(!ioctl(drm,IOC(9,sizeof(c)),&c));}
    close(drm);puts("VULKAN_MULTI_COMPOSITOR_PASS");return 0;
}
