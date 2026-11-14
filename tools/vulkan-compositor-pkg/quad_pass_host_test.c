/* Linux host check of the quad compositor on a real Intel GPU (i915 render
 * node, Mesa). Not run by the nix build (no GPU in the sandbox). It:
 *  1. compares the quad pass with the compute renderer and the CPU oracle
 *     on random scenes (all modes, scales, holes, damage), exactly;
 *  2. composes a 3200x1800 desktop (wallpaper, 2x opaque game, chrome,
 *     taskbar, cursor) with both renderers and reports GPU times for a full
 *     frame, the old footprint damage, a game-only frame and a cursor/clock
 *     frame. Host timings only indicate relative cost.
 * Usage: brook-quad-pass-host-test [random-iterations] */
#define _GNU_SOURCE
#include "renderer.h"
#include "graphics_pass.h"
#include "quad_pass.h"
#include "quad_reference.h"
#include "desktop_tile.h"
#include "desktop_graphics_damage.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define IOC(n,s) ((3u<<30)|((unsigned)(s)<<16)|('d'<<8)|(n))
#define CHECK(x) do{if(!(x)){fprintf(stderr,"QUAD_HOST_FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
struct drm_version_wire {int major,minor,patch;size_t nameLen;char *name;size_t dateLen;char *date;size_t descLen;char *desc;};
struct create {uint64_t size;uint32_t handle,pad;};
struct map_offset {uint32_t handle,pad;uint64_t offset,flags,extensions;};
struct prime {uint32_t handle,flags;int32_t fd;};
struct domain {uint32_t handle,read,write;};
struct Gem {uint32_t handle;int fd;uint64_t size;uint32_t *map;};
static int drm=-1;
static uint32_t rng=0x9e3779b9u;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static uint32_t pick(uint32_t n){return n?next()%n:0;}
static uint64_t now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}

static void open_intel(void){
    for(int n=128;n<136 && drm<0;++n){char path[32];snprintf(path,sizeof(path),"/dev/dri/renderD%d",n);
        int fd=open(path,O_RDWR|O_CLOEXEC);if(fd<0)continue;
        char name[32]={0};struct drm_version_wire v={0};v.name=name;v.nameLen=sizeof(name)-1;
        if(!ioctl(fd,IOC(0x00,sizeof(v)),&v) && !strcmp(name,"i915")){drm=fd;printf("QUAD_HOST_NODE %s\n",path);}
        else close(fd);}
    CHECK(drm>=0);
}
static void gem_new(uint64_t bytes,struct Gem *g){
    struct create c={bytes,0,0};CHECK(!ioctl(drm,IOC(0x5b,sizeof(c)),&c));
    struct map_offset m={c.handle,0,0,2,0};CHECK(!ioctl(drm,IOC(0x64,sizeof(m)),&m));
    void *map=mmap(NULL,c.size,PROT_READ|PROT_WRITE,MAP_SHARED,drm,(off_t)m.offset);CHECK(map!=MAP_FAILED);
    struct prime p={c.handle,O_CLOEXEC|O_RDWR,-1};CHECK(!ioctl(drm,IOC(0x2d,sizeof(p)),&p));
    *g=(struct Gem){c.handle,p.fd,c.size,map};
}
static void gem_free(struct Gem *g){munmap(g->map,g->size);close(g->fd);struct {uint32_t h,pad;} c={g->handle,0};(void)ioctl(drm,IOC(9,sizeof(c)),&c);*g=(struct Gem){0};}
static void gem_cpu(struct Gem *g,int write){struct domain d={g->handle,1,write?1u:0u};CHECK(!ioctl(drm,IOC(0x5f,sizeof(d)),&d));}

static BrookVkRenderer *compute,*batch;static BrookVkQuadPass *quad;
static uint64_t compute_full(const uint32_t *scene,size_t words,struct Gem *src,unsigned count,struct Gem *out){
    struct BrookVkExternalSource s[BROOK_VK_MAX_LAYERS];
    for(unsigned i=0;i<count;++i)s[i]=(struct BrookVkExternalSource){src[i].fd,src[i].size/4,-1};
    CHECK(brook_vk_renderer_submit_batch_output(compute,scene,words,1,s,count,out->fd,(size_t)scene[0]*scene[1])==BROOK_VK_PENDING);
    BrookVkResult r;while((r=brook_vk_renderer_poll(compute))==BROOK_VK_PENDING){}
    CHECK(r==BROOK_VK_COMPLETE);uint64_t ns=0;(void)brook_vk_renderer_gpu_timestamp_ns(compute,&ns);return ns;
}
static uint64_t quad_run(const uint32_t *scene,size_t words,struct Gem *src,const uint32_t *widths,unsigned count,struct Gem *out,
                         const struct QuadPlan *plan,uint64_t *wallNs){
    struct BrookVkQuadSource s[BROOK_VK_MAX_LAYERS];
    for(unsigned i=0;i<count;++i){const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
        s[i]=(struct BrookVkQuadSource){{src[i].fd,-1,l[2],l[3],l[0],l[1],src[i].size},1};}
    (void)widths;
    const struct BrookVkQuadSource o={{out->fd,-1,scene[0],scene[1],0,scene[0],out->size},1};
    const uint64_t start=now_ns();
    const BrookVkResult submitted=brook_vk_quad_pass_submit(quad,scene,words,s,count,&o,plan);
    if(submitted!=BROOK_VK_PENDING){fprintf(stderr,"QUAD_HOST_SUBMIT result=%d draws=%u clears=%u\n",submitted,plan->drawCount,plan->clearCount);
        for(unsigned i=0;i<count;++i){const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
            fprintf(stderr,"  layer %u src %ux%u stride %u used %u\n",i,l[2],l[3],l[1],plan->used[i]);}}
    CHECK(submitted==BROOK_VK_PENDING);
    {BrookVkResult waited=brook_vk_quad_pass_wait(quad,5000000000ull);if(waited!=BROOK_VK_COMPLETE)fprintf(stderr,"QUAD_HOST_WAIT result=%d\n",waited);CHECK(waited==BROOK_VK_COMPLETE);}
    if(wallNs)*wallNs=now_ns()-start;
    uint64_t ns=0;(void)brook_vk_quad_pass_gpu_timestamp_ns(quad,&ns);return ns;
}

enum {RW=256,RH=160,MAXL=8};
static void random_scene(unsigned iteration){
    uint32_t scene[BROOK_VK_SCENE_HEADER+MAXL*BROOK_VK_LAYER_WORDS]={RW,RH,0,0};
    struct QuadRect holes[MAXL];memset(holes,0,sizeof(holes));
    const uint32_t count=1+pick(MAXL);scene[2]=count;
    static struct Gem src[MAXL],a,b;static int ready;
    if(!ready){for(unsigned i=0;i<MAXL;++i)gem_new(96*96*4,&src[i]);gem_new(RW*RH*4,&a);gem_new(RW*RH*4,&b);ready=1;}
    const uint32_t *pixels[MAXL];
    for(uint32_t i=0;i<count;++i){
        uint32_t sw=1+pick(90),sh=1+pick(90),stride=sw;
        /* Strides the driver accepts for linear images (64-byte rows) and
         * tight ones (the importer falls back to the stride width). */
        if(pick(2))stride=(sw+15u)&~15u;
        if(stride>96)stride=96;
        if(sw>stride)sw=stride;
        uint32_t scale=pick(4),dw=sw,dh=sh;
        if(scale==1){dw=2*sw;dh=2*sh;}
        else if(scale==2){dw=3*sw;dh=sh;}
        else if(scale==3){dw=1+pick(200);dh=1+pick(150);}
        uint32_t flags=pick(3)==0?0:BROOK_VK_STRAIGHT_ALPHA;
        if(flags && pick(3)==0)flags|=BROOK_VK_OPAQUE_SOURCE;
        uint32_t opacity=pick(4)==0?255:pick(4)==0?0:pick(256);
        int32_t x=(int32_t)pick(RW+40)-20,y=(int32_t)pick(RH+40)-20;
        struct BrookVkSceneLayer l={0,stride,sw,sh,0,0,sw,sh,x,y,dw,dh,0,0,RW,RH,opacity|flags};
        if(pick(4)==0){l.sourceX=pick(sw);l.sourceY=pick(sh);l.sourceWidthUsed=sw-l.sourceX;l.sourceHeightUsed=sh-l.sourceY;
            if(scale==0){l.width=l.sourceWidthUsed;l.height=l.sourceHeightUsed;}}
        if(pick(3)==0){l.clipX=(int32_t)pick(RW);l.clipY=(int32_t)pick(RH);l.clipWidth=pick(RW);l.clipHeight=pick(RH);}
        gem_cpu(&src[i],1);
        uint32_t *p=src[i].map;for(uint32_t k=0;k<96*96;++k)p[k]=next();
        if(!(l.opacity&BROOK_VK_STRAIGHT_ALPHA))for(uint32_t k=0;k<96*96;++k){uint32_t v=p[k],al=v>>24,c[3];
            if(pick(8)){for(unsigned j=0;j<3;++j){c[j]=(v>>(8*j))&255u;if(c[j]>al)c[j]=al;}p[k]=c[0]|(c[1]<<8)|(c[2]<<16)|(al<<24);}}
        if((l.opacity&BROOK_VK_STRAIGHT_ALPHA) && !(l.opacity&BROOK_VK_OPAQUE_SOURCE) && scale==0 && !l.sourceX && !l.sourceY &&
           sw>4 && sh>4 && pick(2)){
            const uint32_t hx=1+pick(sw/2),hy=1+pick(sh/2),hw=1+pick(sw-hx-1),hh=1+pick(sh-hy-1);
            for(uint32_t r=hy;r<hy+hh;++r)for(uint32_t c=hx;c<hx+hw;++c)p[r*stride+c]&=0x00ffffffu;
            holes[i]=(struct QuadRect){x+(int32_t)hx,y+(int32_t)hy,x+(int32_t)(hx+hw),y+(int32_t)(hy+hh)};
        }
        if(i==0 && pick(2)){l=(struct BrookVkSceneLayer){0,96,96,96,0,0,96,96,0,0,RW,RH,0,0,RW,RH,255};holes[0]=(struct QuadRect){0};}
        memcpy(scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS,&l,sizeof(l));pixels[i]=p;
    }
    struct DesktopDamageSet damage={0};
    const unsigned rects=1+pick(5);
    for(unsigned r=0;r<rects;++r){int64_t x=pick(RW),y=pick(RH);
        desktop_damage_add(&damage,(struct DesktopDamageRect){x,y,x+1+pick(RW),y+1+pick(RH)},RW,RH);}
    if(pick(4)==0){damage.count=1;damage.rects[0]=(struct DesktopDamageRect){0,0,RW,RH};}
    static struct QuadPlan plan;
    CHECK(quad_plan_build_auto(scene,sizeof(scene)/4,holes,&damage,iteration%4!=3,&plan));
    gem_cpu(&b,1);static uint32_t garbage[RW*RH];for(uint32_t k=0;k<RW*RH;++k)b.map[k]=garbage[k]=next();
    (void)compute_full(scene,BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS,src,count,&a);
    (void)quad_run(scene,BROOK_VK_SCENE_HEADER+count*BROOK_VK_LAYER_WORDS,src,NULL,count,&b,&plan,NULL);
    gem_cpu(&a,0);gem_cpu(&b,0);
    for(uint32_t y=0;y<RH;++y)for(uint32_t x=0;x<RW;++x){
        int damaged=0;for(uint32_t r=0;r<damage.count;++r){const struct DesktopDamageRect *d=&damage.rects[r];
            if(x>=d->left && x<d->right && y>=d->top && y<d->bottom)damaged=1;}
        const uint32_t oracle=quad_reference_pixel(scene,pixels,x,y),gpu=a.map[y*RW+x],got=b.map[y*RW+x];
        if(gpu!=oracle){fprintf(stderr,"QUAD_HOST_COMPUTE_ORACLE_MISMATCH iteration=%u x=%u y=%u compute=%08x oracle=%08x\n",iteration,x,y,gpu,oracle);exit(1);}
        if(got!=(damaged?oracle:garbage[y*RW+x])){
            fprintf(stderr,"QUAD_HOST_MISMATCH iteration=%u x=%u y=%u damaged=%d quad=%08x compute=%08x layers=%u\n",
                iteration,x,y,damaged,got,gpu,count);
            for(uint32_t i=0;i<count;++i){const uint32_t *l=scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS;
                fprintf(stderr,"  layer %u src %ux%u stride %u crop %u,%u %ux%u dst %d,%d %ux%u clip %d,%d %ux%u mode %x\n",
                    i,l[2],l[3],l[1],l[4],l[5],l[6],l[7],(int)l[8],(int)l[9],l[10],l[11],(int)l[12],(int)l[13],l[14],l[15],l[16]);}
            exit(1);
        }
    }
}

static int cmp64(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return x<y?-1:x>y;}
static uint64_t median(uint64_t *v,unsigned n){qsort(v,n,sizeof(*v),cmp64);return v[n/2];}
enum {DW=3200,DH=1800,LAYERS=5,REPS=15};
static void desktop(void){
    static struct Gem src[LAYERS],outCompute,outQuad,packed,batchOut;
    const uint32_t gw=1280,gh=720,cw=2568,ch=1478,tw=DW,th=96,kw=24;
    const uint64_t bytes[LAYERS]={(uint64_t)DW*DH*4,(uint64_t)gw*gh*4,(uint64_t)cw*ch*4,(uint64_t)tw*th*4,64*64*4};
    for(unsigned i=0;i<LAYERS;++i){gem_new(bytes[i],&src[i]);gem_cpu(&src[i],1);}
    gem_new((uint64_t)DW*DH*4,&outCompute);gem_new((uint64_t)DW*DH*4,&outQuad);
    for(uint64_t k=0;k<(uint64_t)DW*DH;++k)src[0].map[k]=0xff000000u|(uint32_t)((k*2654435761u)>>8);
    for(uint64_t k=0;k<(uint64_t)gw*gh;++k)src[1].map[k]=(uint32_t)(k*40503u); /* XRGB: alpha ignored */
    for(uint32_t y=0;y<ch;++y)for(uint32_t x=0;x<cw;++x){
        const int interior=x>=4 && x<4+2560 && y>=34 && y<34+1440;
        src[2].map[y*cw+x]=interior?(x*y)&0x00ffffffu:0xff203040u+(x&15u);}
    for(uint64_t k=0;k<(uint64_t)tw*th;++k)src[3].map[k]=(uint32_t)(0xc0000000u|(k&0xffffffu));
    for(uint32_t k=0;k<64*64;++k)src[4].map[k]=(k&7u)?0x80ffffffu:0xff000000u;
    const int32_t gx=320,gy=176;
    uint32_t scene[BROOK_VK_SCENE_HEADER+LAYERS*BROOK_VK_LAYER_WORDS]={DW,DH,LAYERS,0};
    const struct BrookVkSceneLayer layers[LAYERS]={
        {0,DW,DW,DH,0,0,DW,DH,0,0,DW,DH,0,0,DW,DH,255},
        {0,gw,gw,gh,0,0,gw,gh,gx,gy,2*gw,2*gh,0,0,DW,DH,255|BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE},
        {0,cw,cw,ch,0,0,cw,ch,gx-4,gy-34,cw,ch,0,0,DW,DH,255|BROOK_VK_STRAIGHT_ALPHA},
        {0,tw,tw,th,0,0,tw,th,0,DH-th,tw,th,0,0,DW,DH,255|BROOK_VK_STRAIGHT_ALPHA},
        {0,64,kw,kw,0,0,kw,kw,1000,600,kw,kw,0,0,DW,DH,255|BROOK_VK_STRAIGHT_ALPHA}};
    for(unsigned i=0;i<LAYERS;++i)memcpy(scene+BROOK_VK_SCENE_HEADER+i*BROOK_VK_LAYER_WORDS,&layers[i],sizeof(layers[i]));
    struct QuadRect holes[LAYERS]={{0}};holes[2]=(struct QuadRect){gx,gy,gx+2560,gy+1440};
    const size_t words=sizeof(scene)/4;
    /* Exactness of the full desktop frame against compute. */
    static struct QuadPlan plan;
    struct DesktopDamageSet full={1,{{0,0,DW,DH}}};
    CHECK(quad_plan_build(scene,words,holes,&full,1,&plan));
    gem_cpu(&outQuad,1);memset(outQuad.map,0x5a,outQuad.size);
    uint64_t c[REPS],q[REPS],w[REPS];
    for(unsigned i=0;i<REPS;++i)c[i]=compute_full(scene,words,src,LAYERS,&outCompute);
    for(unsigned i=0;i<REPS;++i)q[i]=quad_run(scene,words,src,NULL,LAYERS,&outQuad,&plan,&w[i]);
    gem_cpu(&outCompute,0);gem_cpu(&outQuad,0);
    for(uint64_t k=0;k<(uint64_t)DW*DH;++k)if(outCompute.map[k]!=outQuad.map[k]){
        fprintf(stderr,"QUAD_HOST_DESKTOP_MISMATCH x=%llu y=%llu quad=%08x compute=%08x\n",(unsigned long long)(k%DW),
            (unsigned long long)(k/DW),outQuad.map[k],outCompute.map[k]);exit(1);}
    printf("QUAD_HOST_DESKTOP_EXACT pixels=%u layers_drawn=%u draws=%u shaded=%llu\n",DW*DH,plan.layersDrawn,plan.drawCount,
        (unsigned long long)plan.pixelsShaded);
    const uint64_t computeFull=median(c,REPS),quadFull=median(q,REPS);
    printf("QUAD_HOST_TIMING case=full compute_gpu_us=%llu quad_gpu_us=%llu quad_wall_us=%llu\n",
        (unsigned long long)computeFull/1000,(unsigned long long)quadFull/1000,(unsigned long long)median(w,REPS)/1000);
    /* The renderer's current partial path: footprint damage (game, chrome,
     * taskbar, cursor), culled tiles, backdrop cropped on the CPU. */
    struct DesktopDamageSet footprint={0};
    desktop_damage_add(&footprint,(struct DesktopDamageRect){gx-4,gy-34,gx-4+cw,gy-34+ch},DW,DH);
    desktop_damage_add(&footprint,(struct DesktopDamageRect){0,DH-th,DW,DH},DW,DH);
    desktop_damage_add(&footprint,(struct DesktopDamageRect){1000,600,1000+kw,600+kw},DW,DH);
    size_t sizes[LAYERS],culledWords[LAYERS];for(unsigned i=0;i<LAYERS;++i)sizes[i]=src[i].size/4;
    uint32_t culled[BROOK_VK_SCENE_HEADER+LAYERS*BROOK_VK_LAYER_WORDS],selected[LAYERS];size_t culledCount=0;
    CHECK(desktop_tile_cull_policy(scene,words,sizes,LAYERS,&footprint,culled,sizeof(culled)/4,selected,&culledCount,0));
    uint64_t damagePixels=0;for(uint32_t i=0;i<footprint.count;++i)damagePixels+=(uint64_t)(footprint.rects[i].right-footprint.rects[i].left)*
        (uint64_t)(footprint.rects[i].bottom-footprint.rects[i].top);
    const uint64_t packedBytes=(damagePixels+(uint64_t)footprint.count*DESKTOP_TILE_ALIGNMENT_WORDS)*4;
    gem_new(packedBytes,&packed);gem_new(packedBytes,&batchOut);
    static uint32_t tiles[BROOK_VK_MAX_TILES*BROOK_VK_TILE_STRIDE];struct DesktopPackedTile packedTiles[BROOK_VK_MAX_TILES];
    for(size_t i=0;i<culledCount;++i)culledWords[i]=sizes[selected[i]];
    struct BrookVkExternalSource bs[LAYERS];
    for(unsigned rep=0;rep<REPS;++rep){
        const uint64_t start=now_ns();
        gem_cpu(&packed,1);
        CHECK(desktop_tile_pack(culled,sizeof(culled)/4,culledWords,culledCount,&footprint,src[0].map,packed.map,packed.size/4,
                                tiles,sizeof(tiles)/4,packedTiles,BROOK_VK_MAX_TILES));
        for(size_t i=0;i<culledCount;++i)bs[i]=(struct BrookVkExternalSource){src[selected[i]].fd,src[selected[i]].size/4,-1};
        bs[0]=(struct BrookVkExternalSource){packed.fd,packed.size/4,-1};
        CHECK(brook_vk_renderer_submit_batch_output(batch,tiles,(size_t)footprint.count*BROOK_VK_TILE_STRIDE,footprint.count,bs,culledCount,
                                                    batchOut.fd,batchOut.size/4)==BROOK_VK_PENDING);
        BrookVkResult r;while((r=brook_vk_renderer_poll(batch))==BROOK_VK_PENDING){}
        CHECK(r==BROOK_VK_COMPLETE);(void)brook_vk_renderer_gpu_timestamp_ns(batch,&c[rep]);w[rep]=now_ns()-start;
    }
    const uint64_t computeFoot=median(c,REPS),computeFootWall=median(w,REPS);
    struct DesktopDamageSet aligned;CHECK(desktop_graphics_damage(&footprint,DW,DH,&aligned));
    CHECK(quad_plan_build(scene,words,holes,&aligned,1,&plan));
    for(unsigned i=0;i<REPS;++i)q[i]=quad_run(scene,words,src,NULL,LAYERS,&outQuad,&plan,&w[i]);
    printf("QUAD_HOST_TIMING case=footprint damage_px=%llu compute_gpu_us=%llu compute_wall_us=%llu quad_gpu_us=%llu quad_wall_us=%llu quad_layers=%u quad_shaded=%llu compute_shaded=%llu\n",
        (unsigned long long)damagePixels,(unsigned long long)computeFoot/1000,(unsigned long long)computeFootWall/1000,
        (unsigned long long)median(q,REPS)/1000,(unsigned long long)median(w,REPS)/1000,plan.layersDrawn,
        (unsigned long long)plan.pixelsShaded,(unsigned long long)(damagePixels*culledCount));
    /* Change-based damage: a new game frame; then cursor plus clock. */
    struct DesktopDamageSet game={1,{{gx,gy,gx+2560,gy+1440}}},small={0};
    desktop_damage_add(&small,(struct DesktopDamageRect){990,590,1040,640},DW,DH);
    desktop_damage_add(&small,(struct DesktopDamageRect){3000,DH-th+24,3150,DH-24},DW,DH);
    const struct {const char *name;struct DesktopDamageSet *d;} cases[]={{"game",&game},{"cursor_clock",&small}};
    for(unsigned k=0;k<2;++k){
        CHECK(desktop_graphics_damage(cases[k].d,DW,DH,&aligned));CHECK(quad_plan_build(scene,words,holes,&aligned,1,&plan));
        for(unsigned i=0;i<REPS;++i)q[i]=quad_run(scene,words,src,NULL,LAYERS,&outQuad,&plan,&w[i]);
        printf("QUAD_HOST_TIMING case=%s damage_px=%llu quad_gpu_us=%llu quad_wall_us=%llu layers=%u draws=%u shaded=%llu\n",
            cases[k].name,(unsigned long long)plan.damagePixels,(unsigned long long)median(q,REPS)/1000,
            (unsigned long long)median(w,REPS)/1000,plan.layersDrawn,plan.drawCount,(unsigned long long)plan.pixelsShaded);
    }
    /* After partial frames the whole output must still equal compute. */
    gem_cpu(&outQuad,0);
    for(uint64_t k=0;k<(uint64_t)DW*DH;++k)CHECK(outCompute.map[k]==outQuad.map[k]);
    const struct BrookVkQuadStats s=brook_vk_quad_pass_stats(quad);
    printf("QUAD_HOST_CACHE imports=%llu hits=%llu evictions=%llu submits=%llu\n",(unsigned long long)s.imports,
        (unsigned long long)s.cacheHits,(unsigned long long)s.evictions,(unsigned long long)s.submits);
    for(unsigned i=0;i<LAYERS;++i)gem_free(&src[i]);
    gem_free(&outCompute);gem_free(&outQuad);gem_free(&packed);gem_free(&batchOut);
}

int main(int argc,char **argv){
    const unsigned iterations=argc>1?(unsigned)strtoul(argv[1],NULL,10):300;
    setenv("BROOK_RENDER_GPU_TIMESTAMPS","1",0);
    open_intel();
    compute=brook_vk_renderer_create_multi_output(BROOK_VK_SCENE_HEADER+BROOK_VK_MAX_LAYERS*BROOK_VK_LAYER_WORDS,0);CHECK(compute);
    batch=brook_vk_renderer_create_batch_output(BROOK_VK_MAX_TILES,0);CHECK(batch);
    printf("QUAD_HOST_DEVICE %s\n",brook_vk_renderer_device(compute));
    VkPhysicalDevice physical;VkDevice device;VkQueue queue;uint32_t family;
    CHECK(brook_vk_renderer_graphics_context(compute,&physical,&device,&queue,&family));
    quad=brook_vk_quad_pass_create(physical,device,queue,family);CHECK(quad);
    for(unsigned i=0;i<iterations;++i)random_scene(i);
    printf("QUAD_HOST_RANDOM_EXACT iterations=%u\n",iterations);
    desktop();
    CHECK(brook_vk_quad_pass_destroy(quad));CHECK(brook_vk_renderer_destroy(batch));CHECK(brook_vk_renderer_destroy(compute));
    printf("QUAD_HOST_PASS\n");
    return 0;
}
