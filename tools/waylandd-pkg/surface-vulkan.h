#ifndef BROOK_SURFACE_VULKAN_H
#define BROOK_SURFACE_VULKAN_H
#include "surface.h"
#include "scene.h"
#include "gpu_window_source_abi.h"
#ifdef BROOK_WAYLAND_VULKAN_TEST
static unsigned brook_vulkan_test_success,brook_vulkan_test_fallback;
#endif

/* Experimental single-layer bridge. The producer remains retained through
 * both GPU completion and the later CPU fallback snapshot. The result is 1
 * only after publication, -1 if GPU use is uncertain, and 0 for fallback. */
static int surface_vulkan_direct(struct brook_surface *root,struct brook_image *image,int sourceFd,int syncFd) {
    static BrookVkSurface *direct;
    static int disabled;
    const char *enabled=getenv("BROOK_WAYLAND_DIRECT_SOURCE");
    if(disabled || !enabled || strcmp(enabled,"1") || !root || !image || !image->source ||
       !image->source->dmabuf || sourceFd<0 || syncFd<0 || !root->xdg_toplevel || !root->xdg_acked)return 0;
    struct brook_draw draws[64];unsigned n=0;
    if(!surface_layers(root,draws,&n) || n!=1 || draws[0].view->image!=image)return 0;
    const struct brook_view *view=draws[0].view;const struct brook_geometry *g=&view->geometry;
    const struct brook_dmabuf_buffer *db=image->source->dmabuf;const uint32_t scale=surface_output_scale(root);
    if(g->transform || ((g->sx|g->sy|g->sw|g->sh)&255) || (db->stride&3) || (db->offset&3) || db->modifier ||
       (db->fourcc!=0x34325241 && db->fourcc!=0x34325258) || g->w>16384/scale || g->h>16384/scale)return 0;
    off_t bytes=lseek(sourceFd,0,SEEK_END);if(bytes<=0 || (bytes&3) || lseek(sourceFd,0,SEEK_SET)!=0)return 0;
    if(!surface_window(root,(struct brook_view *)view) || !root->wm_id || !root->vfb)return 0;
    struct stat source={0};BrookWindowState window={0};
    if(fstat(sourceFd,&source) || !source.st_ino ||
       syscall(BROOK_SYS_WM_WINDOW_STATE,(long)root->wm_id,0L,(long)&window,(long)sizeof(window),0L,0L) ||
       window.version!=BROOK_WINDOW_STATE_VERSION || window.window!=root->wm_id || !root->tree.current)return 0;
    struct BrookGpuWindowIdentity identity={root->wm_id,0,window.incarnation,root->tree.current->commit,
        (uint64_t)source.st_dev,(uint64_t)source.st_ino};
    if(!brook_gpu_window_identity_valid(&identity) || window.width!=root->vfb_w || window.height!=root->vfb_h)return 0;
    size_t pixels=(size_t)root->vfb_w*root->vfb_h;
    if(root->composite_pixels<pixels){uint32_t *next=realloc(root->composite,pixels*4);if(!next)return 0;
        root->composite=next;root->composite_pixels=pixels;}
    if(!direct){direct=brook_vk_surface_create(0);if(!direct){disabled=1;return 0;}}
    struct BrookVkSurfaceExternalLayer layer={sourceFd,syncFd,(size_t)bytes/4,db->offset/4,db->stride/4,
        image->width,image->height,g->sx/256,g->sy/256,g->sw/256,g->sh/256,0,0,g->w*scale,g->h*scale,
        0,0,root->vfb_w,root->vfb_h,255,db->fourcc==0x34325258?(BROOK_VK_STRAIGHT_ALPHA|BROOK_VK_OPAQUE_SOURCE):0};
    BrookVkResult result=brook_vk_surface_compose_external(direct,root->vfb_w,root->vfb_h,&layer,1,root->composite,pixels);
    if(result==BROOK_VK_UNCERTAIN){disabled=1;fprintf(stderr,"[waylandd] direct producer GPU read uncertain; release withheld\n");return -1;}
    if(result!=BROOK_VK_COMPLETE)return 0;
    BrookWindowState current={0};struct stat currentSource={0};
    if(!root->tree.current || root->tree.current->value!=view || root->tree.current->commit!=identity.commit ||
       syscall(BROOK_SYS_WM_WINDOW_STATE,(long)identity.window,(long)identity.incarnation,(long)&current,(long)sizeof(current),0L,0L) ||
       current.version!=BROOK_WINDOW_STATE_VERSION || current.window!=identity.window || current.incarnation!=identity.incarnation ||
       current.width!=root->vfb_w || current.height!=root->vfb_h || fstat(sourceFd,&currentSource))return 0;
    struct BrookGpuWindowIdentity confirmed={current.window,0,current.incarnation,root->tree.current->commit,
        (uint64_t)currentSource.st_dev,(uint64_t)currentSource.st_ino};
    if(!brook_gpu_window_identity_equal(&identity,&confirmed))return 0;
    if(!surface_begin_source_write(root,NULL))return 0;
    for(unsigned y=0;y<root->vfb_h;++y)for(unsigned x=0;x<root->vfb_w;++x)
        root->vfb[(size_t)y*root->vfb_stride+x]=pixel_straight(root->composite[(size_t)y*root->vfb_w+x]);
    wm_signal_dirty(root->wm_id);return 1;
}

/* Single-threaded adapter. Unsupported or uncertain work returns 0 and the
 * existing CPU composition path owns publication. Vulkan only sees copies. */
static int surface_vulkan_compose(struct brook_surface *root,const struct brook_draw *draws,
                                  unsigned count,uint32_t scale) {
    static BrookVkSurface *renderer;
    static int initialized,disabled,announced;
    if(!initialized) {
        initialized=1;
        const char *enabled=getenv("BROOK_WAYLAND_VULKAN_COMPOSE");
        if(!enabled || strcmp(enabled,"1"))disabled=1;
        else {
#ifdef BROOK_WAYLAND_VULKAN_TEST
            renderer=brook_vk_surface_create(1);
#else
            renderer=brook_vk_surface_create(0);
#endif
            if(!renderer)disabled=1;
        }
    }
    if(disabled || count>64)return 0;
    struct BrookVkSurfaceLayer layers[64];unsigned n=0;
    for(unsigned i=0;i<count;++i){
        const struct brook_draw *d=draws+i;const struct brook_view *view=d->view;
        const struct brook_image *image=view->image;const struct brook_geometry *g=&view->geometry;
        int64_t x=d->x*scale,y=d->y*scale;
        if(!image->pixels || g->transform || ((g->sx|g->sy|g->sw|g->sh)&255) ||
           x<-(int64_t)16384 || x>16384 || y<-(int64_t)16384 || y>16384) {
#ifdef BROOK_WAYLAND_VULKAN_TEST
            ++brook_vulkan_test_fallback;
#endif
            return 0;
        }
        layers[n++]=(struct BrookVkSurfaceLayer){image->pixels,image->width,image->height,
            g->sx/256,g->sy/256,g->sw/256,g->sh/256,(int32_t)x,(int32_t)y,g->w*scale,g->h*scale};
    }
    BrookVkResult result=brook_vk_surface_compose(renderer,root->vfb_w,root->vfb_h,layers,n,
        root->composite,(size_t)root->vfb_w*root->vfb_h);
    if(result==BROOK_VK_UNCERTAIN) {
        disabled=1;
        fprintf(stderr,"[waylandd] Vulkan composition uncertain; renderer quarantined, using CPU\n");
    }
    if(result==BROOK_VK_COMPLETE && !announced) {
        announced=1;
        fprintf(stderr,"[waylandd] Vulkan surface composition active on %s\n",brook_vk_surface_device(renderer));
    }
#ifdef BROOK_WAYLAND_VULKAN_TEST
    if(result==BROOK_VK_COMPLETE) {
        if(!brook_vulkan_test_success)fprintf(stderr,"WAYLAND_VULKAN_DEVICE %s\n",brook_vk_surface_device(renderer));
        ++brook_vulkan_test_success;
    } else ++brook_vulkan_test_fallback;
#endif
    return result==BROOK_VK_COMPLETE;
}
#endif
