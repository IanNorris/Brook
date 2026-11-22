#ifndef BROOK_SURFACE_SCENE_H
#define BROOK_SURFACE_SCENE_H
#include "surface-blit.h"
/* Single-threaded Wayland adapter. Pixel contents are immutable. The optional
 * GPU storage cache can migrate only after a verified clone; scene snapshots
 * retain logical images and never retain producer resource numbers alone. */
struct brook_scene_owner { unsigned refs;int fd; };
struct brook_image {
    unsigned refs;
    uint32_t width,height,format;
    uint32_t *pixels;
    /* Every pixel has alpha 255 (XRGB SHM snapshots force it), so CPU
     * composition may copy instead of blending. */
    int opaque;
    struct brook_buffer *source;
    struct brook_scene_owner *owner;
    uint64_t version;
    int uncertain;
    int pending_cpu,pending_fd;
    int producer_fence,producer_fence_required,direct_attempted;
    int offer_state;
    /* Version 2 GPU channel: the renderer reported a frame sampling this
     * offer (gpu_presented); want_pixels requests a CPU snapshot while the
     * renderer still retains it. offer_ms/offer_geometry describe the offer. */
    int gpu_presented,want_pixels;
    uint32_t offer_ms;
    struct brook_geometry offer_geometry;
    uint32_t offer_sent_ms,offer_ack_ms;
    uint32_t trace_queued_ms,trace_first_poll_ms,trace_fence_ms;
    struct BrookGpuWindowIdentity offer_identity;
    struct brook_image *pending_next;
    /* Version 3 layered offers: groups currently holding this image, the
     * tree commit the offer stood for, and, for CPU pixels, a GEM copy
     * uploaded once (gem_fd prime fd, gem_handle) and its idle fence. */
    unsigned group_refs;uint32_t group_wait_ms,queued_ms;
    uint64_t offer_tree_commit;
    int gem_fd,gem_fence;uint32_t gem_handle;uint64_t gem_words;
    uint64_t pace_commit_us,pace_offer_us; /* frame pacing (waylandd.c) */
    int pace_x11;
    uint64_t buffer_trace_epoch, buffer_trace_attachment, buffer_trace_cycle;
    unsigned buffer_trace_serial, buffer_trace_surface;
    int buffer_trace_producer_ready, buffer_trace_offer_ack;
};
static void image_buffer_trace(const struct brook_image *image, enum buffer_trace_kind kind,
                               const struct BrookGpuWindowIdentity *offer)
{
    if (!g_buffer_trace.pid || image->buffer_trace_epoch != g_buffer_trace.epoch) return;
    buffer_trace_record(kind, image->buffer_trace_serial, image->buffer_trace_attachment,
                        image->buffer_trace_cycle, image->buffer_trace_surface,
                        image->source ? image->source->uses : 0, offer);
}
static void image_trace_producer_ready(struct brook_image *image)
{
    if (!g_buffer_trace.pid || image->buffer_trace_producer_ready) return;
    image_buffer_trace(image, BUFFER_PRODUCER_READY, NULL);
    image->buffer_trace_producer_ready = 1;
}
static void image_gem_free(struct brook_image *image);
static long scene_ctl(int fd,unsigned op,const void *arg,size_t size) {
    errno=0;
    long result=syscall(0xB07,(long)fd,(long)op,(long)arg,(long)size,0L,0L);
    return result==-1?-errno:result;
}
static void scene_owner_unref(struct brook_scene_owner *owner) {
    if (owner && !--owner->refs) { close(owner->fd);free(owner); }
}
static void image_ref(void *value) { if (value) ++((struct brook_image *)value)->refs; }
static void image_unref(void *value) {
    struct brook_image *image=value;
    if (!image || --image->refs) return;
    if (image->owner) {
        scene_ctl(image->owner->fd,SCENE_CTL_RELEASE,(void *)(uintptr_t)image->version,0);
        scene_owner_unref(image->owner);
    }
    if (image->source) {
        if (!image->uncertain) buffer_release(image->source);
        buffer_drop(image->source);
    }
    image_gem_free(image);
    free(image->pixels);free(image);
}
static struct brook_image *image_snapshot(const uint8_t *src,int w,int h,int stride,uint32_t format) {
    if (!src || w<=0 || h<=0 || w>16384 || h>16384 || stride<0 ||
        (uint64_t)stride<(uint64_t)w*4 || (format!=WL_SHM_FORMAT_ARGB8888 && format!=WL_SHM_FORMAT_XRGB8888)) return NULL;
    struct brook_image *image=calloc(1,sizeof(*image));if (!image) return NULL;
    image->pixels=malloc((size_t)w*h*4);
    if (!image->pixels) { free(image);return NULL; }
    image->refs=1;image->width=w;image->height=h;image->format=format==WL_SHM_FORMAT_ARGB8888?1:2;image->opaque=image->format==2;
    for (int y=0;y<h;++y) {
        const uint32_t *row=(const uint32_t *)(src+(size_t)y*stride);
        uint32_t *out=image->pixels+(size_t)y*w;
        if (image->format==1) memcpy(out,row,(size_t)w*4);
        else for (int x=0;x<w;++x) out[x]=row[x]|0xff000000u;
    }
    return image;
}
/* Each commit retains its own geometry while sharing immutable pixel storage.
 * Metadata-only commits therefore neither capture nor release a producer. */
struct brook_view {
    unsigned refs;
    struct brook_image *image;
    struct brook_geometry geometry;
    /* The client's opaque region covers the whole surface: the compositor
     * may ignore alpha (wl_surface.set_opaque_region). */
    int opaque;
};
static void view_ref(void *value) { if(value) ++((struct brook_view *)value)->refs; }
static void view_unref(void *value) {
    struct brook_view *view=value;
    if (!view || --view->refs) return;
    image_unref(view->image);free(view);
}
static unsigned long long g_opaque_views;
static int surface_opaque_covers(const struct brook_surface *s,uint32_t w,uint32_t h){
    if(!s->opaque_set || !w || !h || s->opaque_w<=0 || s->opaque_h<=0)return 0;
    const int64_t x0=s->opaque_x>0?s->opaque_x:0,y0=s->opaque_y>0?s->opaque_y:0;
    int64_t x1=(int64_t)s->opaque_x+s->opaque_w,y1=(int64_t)s->opaque_y+s->opaque_h;
    if(x1>(int64_t)w)x1=w;
    if(y1>(int64_t)h)y1=h;
    return x0==0 && y0==0 && x1==(int64_t)w && y1==(int64_t)h;
}
static void surface_tree_apply(struct bt_node *node,void *value) {
    struct brook_surface *s=node->user;struct brook_view *view=value;
    struct brook_image *image=view?view->image:NULL;
    s->image=image?image->pixels:NULL;s->image_w=view?view->geometry.w:0;s->image_h=view?view->geometry.h:0;
    s->sub_x=node->x;s->sub_y=node->y;
    struct brook_frame_callback *frame,*next;
    wl_list_for_each_safe(frame,next,&s->waiting_frames,link) {
        if (frame->commit>node->current->commit) continue;
        wl_list_remove(&frame->link);wl_list_insert(s->committed_frames.prev,&frame->link);
    }
}
static void surface_tree_free(struct bt_node *node) { free(node->user); }
static const struct bt_ops surface_tree_ops={view_ref,view_unref,surface_tree_apply,surface_tree_free};
static void surface_tree_init(struct brook_surface *s) {
    bt_node_init(&s->tree,&surface_tree_ops,s);
    wl_list_init(&s->pending_frames);wl_list_init(&s->waiting_frames);wl_list_init(&s->committed_frames);
}
static int surface_tree_commit(struct brook_surface *s,struct brook_image *image) {
    struct brook_view *view=NULL;
    struct brook_geometry geometry;
    int error=brook_geometry_resolve(&s->geometry,image?image->width:0,image?image->height:0,&geometry);
    if (error) {
        if (s->viewport && error!=BROOK_GEOMETRY_BUFFER_SIZE) wl_resource_post_error(s->viewport,
            error==BROOK_GEOMETRY_BOUNDS?WP_VIEWPORT_ERROR_OUT_OF_BUFFER:WP_VIEWPORT_ERROR_BAD_SIZE,
            "invalid committed viewport geometry");
        else wl_resource_post_error(s->resource,WL_SURFACE_ERROR_INVALID_SIZE,"invalid buffer geometry");
        return -1;
    }
    if (image) {
        view=calloc(1,sizeof(*view));if (!view) return 0;
        view->refs=1;view->image=image;view->geometry=geometry;image_ref(image);
        /* wl_region is in surface-local logical coordinates, the same units
         * as the view's destination size (geometry.w/h, before output
         * scale). Clamped to the surface, one rectangle that contains it
         * makes the whole surface opaque (Chrome sends its initial window
         * size, larger than the surface after a resize). */
        view->opaque=surface_opaque_covers(s,geometry.w,geometry.h);
        if(view->opaque)++g_opaque_views;
    }
    struct brook_frame_callback *frame;
    uint32_t now=g_now_ms();
    wl_list_for_each(frame,&s->pending_frames,link) {
        frame->commit=s->tree.sequence+1;frame->committed_ms=now;
    }
    wl_list_insert_list(s->waiting_frames.prev,&s->pending_frames);wl_list_init(&s->pending_frames);
    int result=bt_commit(&s->tree,view);view_unref(view);return result;
}
static void surface_unmap(struct brook_surface *s) {
    pointer_constraints_unmap(s,0);
    seat_clients_unmap_surface(s);
    if (s->role==4) xwayland_native_unmapped(s);
    if (s->wm_id && (s->xdg_toplevel || s->xdg_popup)) {
        s->xdg_initial_commit_done=0;s->xdg_acked=0;
    }
    memset(&s->size_hints,0,sizeof(s->size_hints));memset(&s->pending_size_hints,0,sizeof(s->pending_size_hints));s->size_hints_pending=0;
    s->toplevel_maximized=s->toplevel_resizing=s->toplevel_fullscreen=0;s->configure_width=s->configure_height=0;s->configure_size_pending=s->configure_size_acked=0;s->configure_size_serial=0;
    scene_owner_unref(s->scene);s->scene=NULL;s->scene_disabled=0;
    if (s->wm_id) wm_destroy_window(s->wm_id);
    s->idle_inhibit_sent=0; /* the kernel drops the count with the window */
    s->wm_id=0;s->vfb=NULL;s->vfb_w=s->vfb_h=s->vfb_stride=0;s->deco_sent_mode=-1;
    s->strip_due=3;s->strip_trailing=0;s->strip_style=0;strip_retry_drop(s);
}
static int surface_window(struct brook_surface *s,struct brook_view *view) {
    const int xwayland=s->role==4;
    if (!view || (xwayland && !xwayland_surface_ready(s))) {
        if (s->xdg_toplevel || s->xdg_popup || xwayland) surface_unmap(s);
        return 1;
    }
    if (!(s->xdg_toplevel || s->xdg_popup || xwayland)) return 1;
    if (!xwayland && !s->xdg_acked) return 1;
    const uint32_t scale=surface_output_scale(s);
    if (view->geometry.w>16384/scale || view->geometry.h>16384/scale) return 0;
    const uint32_t width=view->geometry.w*scale,height=view->geometry.h*scale;
    struct brook_wm_create_out out={0};
    if (!s->wm_id) {
        int csd=s->xdg_popup || s->deco_csd || (s->xdg_toplevel && !s->deco_negotiated);
        unsigned flags=(csd?BROOK_WM_CREATE_FLAG_CSD:0)|(s->xdg_popup?BROOK_WM_CREATE_FLAG_NO_FOCUS:0);
        if (xwayland) flags=xwayland_surface_flags(s);
        if (wm_create_window(width,height,s->title[0]?s->title:NULL,flags,&out) || !out.wm_id) return 0;
        s->wm_id=out.wm_id;s->scene_disabled=0;
        wm_register_client(s->wm_id,s->resource);
        if (s->app_id[0]) wm_set_app_id(s->wm_id,s->app_id);
        s->deco_pending=0;s->deco_sent_mode=csd?1:0;s->deco_sent_insets=s->deco_sent_height=s->deco_sent_style=0;popup_apply_position(s);
    } else if (s->vfb_w!=width || s->vfb_h!=height) {
        if (wm_resize_vfb(s->wm_id,width,height,&out) || !out.vfb_user) return 0;
    } else return 1;
    s->vfb=(uint32_t *)(uintptr_t)out.vfb_user;s->vfb_stride=out.vfb_stride;
    s->vfb_w=width;s->vfb_h=height;s->strip_due=3;
    if (s->xdg_toplevel && s->deco_csd) surface_sync_decoration(s,0);
    if (xwayland) xwayland_surface_created(s);
    toplevel_apply_fullscreen(s);
    return 1;
}
static int scene_ensure(struct brook_surface *s) {
    if (s->scene || s->scene_disabled) return 1;
    errno=0;long fd=syscall(0xB06,(long)s->wm_id,(long)SCENE_ABI_VERSION,0L,0L,0L,0L);
    long error=fd==-1?-errno:fd;
    if (error<0) {
        if (error==-ENOSYS || error==-EOPNOTSUPP) { s->scene_disabled=1;return 1; }
        return 0;
    }
    struct brook_scene_owner *owner=calloc(1,sizeof(*owner));
    if (!owner) { close((int)fd);return 0; }
    owner->refs=1;owner->fd=(int)fd;s->scene=owner;return 1;
}
static int image_gpu(struct brook_image *image,struct brook_scene_owner *owner) {
    if (image->uncertain) return 0;
    if (image->owner==owner) return 1;
    long version;
    if (image->owner) {
        struct SceneCloneDesc desc={image->owner->fd,0,image->version};
        version=scene_ctl(owner->fd,SCENE_CTL_CLONE,&desc,sizeof(desc));
    } else if (image->source) {
        const struct brook_dmabuf_buffer *db=image->source->dmabuf;
        struct SceneDmabufCaptureDesc desc={g_drm_fd,db->bo_handle,image->width,image->height,
                                           db->fourcc,db->stride,db->offset,0,db->modifier};
        version=scene_ctl(owner->fd,SCENE_CTL_CAPTURE_DMABUF,&desc,sizeof(desc));
    } else {
        struct SceneUploadDesc desc={(uintptr_t)image->pixels,(uint64_t)image->width*image->height*4,image->width,image->height,image->width*4,image->format};
        version=scene_ctl(owner->fd,SCENE_CTL_UPLOAD,&desc,sizeof(desc));
    }
    if (version<=0) {
        fprintf(stderr,"[waylandd] retained image capture failed: result=%ld size=%ux%u source=%s\n",
                version,image->width,image->height,image->source?"dmabuf":"snapshot");
        if (version==-EIO) { image->uncertain=1;if (image->source) image->source->uncertain=1; }return 0;
    }
    if (image->owner) {
        scene_ctl(image->owner->fd,SCENE_CTL_RELEASE,(void *)(uintptr_t)image->version,0);
        scene_owner_unref(image->owner);
    }
    image->owner=owner;++owner->refs;image->version=(uint64_t)version;
    if (image->source) { buffer_release(image->source);buffer_drop(image->source);image->source=NULL; }
    return 1;
}
struct brook_draw { struct brook_view *view;int64_t x,y; };
static int gpu_window_offer_layers(struct brook_surface *root,const struct brook_draw *draws,unsigned count);
/* Every traversed node is mapped and contributes a layer. Thus the backend's
 * 64-layer bound also bounds this stack, independently of protocol tree depth. */
static int surface_layers(struct brook_surface *root,struct brook_draw *draw,unsigned *count) {
    struct frame { struct bt_node *node;size_t index;int own;int64_t x,y; } stack[65];
    unsigned depth=1;*count=0;stack[0]=(struct frame){&root->tree,0,0,0,0};
    while (depth) {
        struct frame *f=&stack[depth-1];struct bt_state *state=f->node->current;
        if (!state || !state->value) { --depth;continue; }
        if (!f->own && (f->index==state->count || !state->children[f->index].below)) {
            if (*count==64) return 0;
            draw[(*count)++]=(struct brook_draw){state->value,f->x,f->y};f->own=1;continue;
        }
        if (f->index==state->count) { --depth;continue; }
        struct bt_child *child=&state->children[f->index++];
        if (!bt_child_valid(f->node,child)) continue;
        if (depth==65) return 0;
        stack[depth++]=(struct frame){child->node,0,0,f->x+child->x,f->y+child->y};
    }
    return 1;
}
/* Reference implementations, kept for tests; composition uses surface-blit.h. */
__attribute__((unused)) static uint32_t pixel_over(uint32_t src, uint32_t dst) {
    unsigned a = src >> 24, inv = 255 - a;
    if (a == 255) return src;
    if (!a) return dst;
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        unsigned v = ((src >> shift) & 255) + (((dst >> shift) & 255) * inv + 127) / 255;
        out |= (v > 255 ? 255 : v) << shift;
    }
    return out;
}
__attribute__((unused)) static uint32_t pixel_straight(uint32_t px) {
    unsigned a = px >> 24;
    if (!a || a == 255) return a ? px : 0;
    uint32_t out = px & 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned v = (((px >> shift) & 255) * 255 + a / 2) / a;
        out |= (v > 255 ? 255 : v) << shift;
    }
    return out;
}
/* 2 means the OS invalidated an earlier tagged snapshot, 1 means this
 * provider is using an older kernel/no tagged channel, 0 means retry later. */
static int surface_begin_source_write(struct brook_surface *root,uint64_t *incarnation){
    if(incarnation)*incarnation=0;
#ifdef BROOK_WAYLAND_SOURCE_COMMIT_TAG
    const char *channel=getenv("BROOK_WAYLAND_GPU_SOCKET");
    if(channel && *channel){
        BrookWindowState state={0};
        if(syscall(BROOK_SYS_WM_WINDOW_STATE,(long)root->wm_id,0L,(long)&state,(long)sizeof(state),0L,0L))return 0;
        errno=0;
        if(!syscall(BROOK_SYS_WM_BEGIN_SOURCE_WRITE,(long)root->wm_id,(long)state.incarnation,0L,0L,0L,0L)){
            if(incarnation)*incarnation=state.incarnation;
            return 2;
        }
        if(errno!=ENOSYS)return 0;
    }
#else
    (void)root;
#endif
    return 1;
}
#ifdef BROOK_WAYLAND_VULKAN_COMPOSITOR
#include "surface-vulkan.h"
#endif
/* Per-window layer summary, kept for the status file (waylandd_status_write)
 * and, with BROOK_WAYLAND_LAYER_TRACE=1, logged once per second per window or
 * whenever the layer signature changes. Diagnostic only. */
static unsigned long long g_layer_readback_bytes,g_layer_offer_frames;
enum {LAYER_PATH_SCENE,LAYER_PATH_SINGLE,LAYER_PATH_GENERAL,LAYER_PATH_VULKAN,LAYER_PATH_PENDING,LAYER_PATH_GPU_LAYERS,LAYER_PATHS};
static unsigned long long g_layer_paths[LAYER_PATHS];
static const char *const g_layer_path_names[LAYER_PATHS]={"scene","cpu_single","cpu_general","vulkan_compose","pending","gpu_layers"};
struct layer_summary {uint32_t wm,count,vfb_w,vfb_h,scale,ms;int path;uint64_t sig;
    struct {char kind;uint32_t w,h,dw,dh,fmt;int opaque,offer;int64_t x,y;uint32_t sw,sh;} l[4];};
static struct layer_summary g_layer_windows[16];
static void surface_layer_trace(const struct brook_surface *root,const struct brook_draw *draws,unsigned count,int path){
    static int enabled=-1;
    if(enabled<0){const char *e=getenv("BROOK_WAYLAND_LAYER_TRACE");enabled=e && !strcmp(e,"1");}
    if(path>=0 && path<LAYER_PATHS)++g_layer_paths[path];
    uint64_t sig=1469598103934665603ull^(uint64_t)path;
    for(unsigned i=0;i<count;++i){const struct brook_view *v=draws[i].view;
        uint64_t parts[5]={(uint64_t)v->geometry.w<<32|v->geometry.h,(uint64_t)v->image->width<<32|v->image->height,
            (uint64_t)(uint32_t)draws[i].x<<32|(uint32_t)draws[i].y,(uint64_t)v->geometry.sw<<32|v->geometry.sh,
            (uint64_t)(v->image->source && v->image->source->dmabuf)<<1|(uint64_t)v->image->opaque};
        for(unsigned k=0;k<5;++k){sig^=parts[k];sig*=1099511628211ull;}}
    struct layer_summary *w=&g_layer_windows[root->wm_id%16];
    const uint32_t now=g_now_ms();
    const int changed=w->wm!=root->wm_id || w->sig!=sig;
    w->wm=root->wm_id;w->count=count;w->vfb_w=root->vfb_w;w->vfb_h=root->vfb_h;w->path=path;
    w->scale=surface_output_scale((struct brook_surface *)root);
    for(unsigned i=0;i<count && i<4;++i){const struct brook_view *v=draws[i].view;const struct brook_image *im=v->image;
        w->l[i].kind=im->pending_cpu?'p':im->source && im->source->dmabuf?'d':im->pixels?'s':'n';
        w->l[i].w=im->width;w->l[i].h=im->height;w->l[i].dw=v->geometry.w;w->l[i].dh=v->geometry.h;w->l[i].fmt=im->format;
        w->l[i].opaque=im->opaque | (v->opaque<<1);w->l[i].offer=im->offer_state;w->l[i].x=draws[i].x;w->l[i].y=draws[i].y;
        w->l[i].sw=v->geometry.sw;w->l[i].sh=v->geometry.sh;}
    if(!enabled || (!changed && now-w->ms<1000))return;
    w->sig=sig;w->ms=now;
    fprintf(stderr,"[waylandd] LAYERS wm=%u vfb=%ux%u scale=%u count=%u path=%s readback_bytes=%llu\n",
        root->wm_id,root->vfb_w,root->vfb_h,w->scale,count,path>=0 && path<LAYER_PATHS?g_layer_path_names[path]:"?",g_layer_readback_bytes);
    for(unsigned i=0;i<count;++i){const struct brook_view *v=draws[i].view;const struct brook_image *im=v->image;
        const char *kind=im->pending_cpu?"dmabuf-pending":im->source && im->source->dmabuf?"dmabuf":im->pixels?"pixels":"none";
        fprintf(stderr,"[waylandd]   layer%u kind=%s buf=%ux%u fmt=%u opaque=%d at=%lld,%lld dst=%ux%u src24.8=%u,%u,%u,%u transform=%u offer=%d\n",
            i,kind,im->width,im->height,im->format,im->opaque,(long long)draws[i].x,(long long)draws[i].y,
            v->geometry.w,v->geometry.h,v->geometry.sx,v->geometry.sy,v->geometry.sw,v->geometry.sh,v->geometry.transform,im->offer_state);}
}
static int surface_render(struct brook_surface *root) {
    if ((root->xdg_toplevel || root->xdg_popup) && !root->xdg_acked) return 1;
    struct brook_view *base=root->tree.current?root->tree.current->value:NULL;
    if (root->role==4 && base && !xwayland_accept_view(root,base->geometry.w,base->geometry.h)) return 1;
    struct brook_draw draws[64];unsigned count=0;
    if (!surface_layers(root,draws,&count)) return 0;
    /* Several layers: offer them to the renderer as one layered commit
     * (version 3). 2 means a DMA-BUF layer's producer fence is not known
     * yet; the Intel queue offers the commit when it is. */
    if(count>=2 && root->wm_id && root->vfb){
        const int layered=gpu_window_offer_layers(root,draws,count);
        if(layered==1){surface_layer_trace(root,draws,count,LAYER_PATH_GPU_LAYERS);return 1;}
        if(layered==2)return 1;
    }
    /* Keep the previously published frame until every current producer is
     * ready. The queue owns pending images even if their surface is removed. */
    for(unsigned i=0;i<count;++i)if(draws[i].view->image->pending_cpu){
        surface_layer_trace(root,draws,count,LAYER_PATH_PENDING);
        /* An offered GPU source can stand for the window only as its sole,
         * unchanged root layer. Anything else needs CPU pixels now, without
         * waiting for the renderer to give the source back. */
        /* That includes an image held by a presented layered commit: when
         * the window can no longer be offered as layers (a new layer shape,
         * a CPU fallback for another layer) nothing newer would replace that
         * commit, and the renderer would keep showing it while the client
         * keeps drawing (frozen content). Snapshot every such layer. */
        for(unsigned j=i;j<count;++j){struct brook_image *held=draws[j].view->image;
            if(held->pending_cpu && held->offer_state==1 && (held->group_refs || count!=1 || draws[0].view!=base ||
               memcmp(&base->geometry,&held->offer_geometry,sizeof(base->geometry))))intel_image_want_pixels(held);}
        return 1;
    }
    if (!surface_window(root,base)) return 0;
    if (!root->wm_id || !root->vfb) return 1;
    if (!scene_ensure(root)) return 0;
    const uint32_t scale=surface_output_scale(root);
    /* Match the native scene-service bound in both backends, including
     * partially visible subsurfaces rather than only the root window. */
    for(unsigned i=0;i<count;++i)
        if(draws[i].view->geometry.w>16384/scale || draws[i].view->geometry.h>16384/scale) return 0;
    uint64_t writeIncarnation=0;
    int writeGuard=0;
    /* The native scene service does not yet expose a begin-write/commit tag
     * transaction. Keep channel-backed windows on the tagged CPU path. */
    if (root->scene && !(getenv("BROOK_WAYLAND_GPU_SOCKET") && *getenv("BROOK_WAYLAND_GPU_SOCKET"))) {
        struct SceneLayerDesc layers[64];unsigned n=0;
        for (unsigned i=0;i<count;++i) {
            struct brook_draw *d=&draws[i];struct brook_view *view=d->view;
            struct brook_image *image=view->image;const struct brook_geometry *g=&view->geometry;
            int64_t dx=d->x*scale,dy=d->y*scale;
            uint32_t dw=g->w*scale,dh=g->h*scale;
            if (dx>=root->vfb_w || dy>=root->vfb_h || dx+dw<=0 || dy+dh<=0) continue;
            if (!image_gpu(image,root->scene)) return 0;
            layers[n++]=(struct SceneLayerDesc){image->version,g->sx,g->sy,g->sw,g->sh,(int32_t)dx,(int32_t)dy,dw,dh,255,0,0,root->vfb_w,root->vfb_h,SCENE_LAYER_FIXED8|(g->transform<<SCENE_LAYER_TRANSFORM_SHIFT)};
        }
        surface_layer_trace(root,draws,count,LAYER_PATH_SCENE);
        struct ScenePresentDesc desc={(uintptr_t)layers,n,root->vfb_w,root->vfb_h,0};
        long result=scene_ctl(root->scene->fd,SCENE_CTL_PRESENT,&desc,sizeof(desc));
        // Native resize/content generation changes can safely discard a frame.
        // Keep the captured versions; the next commit/configure can redraw.
        return result==0 || result==-116;
    }
    /* A single full-size retained image over transparent black needs no
     * intermediate scene or alpha blend. Preserve straight-alpha conversion
     * and the same source-write/commit protocol as the general path. */
    if(count==1 && draws[0].x==0 && draws[0].y==0){
        const struct brook_view *view=draws[0].view;
        const struct brook_geometry *g=&view->geometry;
        const struct brook_image *image=view->image;
        if(image->pixels && image->format==1 && !g->transform && !g->sx && !g->sy &&
           g->sw==(int64_t)image->width*256 && g->sh==(int64_t)image->height*256 &&
           g->w*scale==image->width && g->h*scale==image->height &&
           root->vfb_w==image->width && root->vfb_h==image->height){
            writeGuard=surface_begin_source_write(root,&writeIncarnation);if(!writeGuard)return 1;
            surface_layer_trace(root,draws,count,LAYER_PATH_SINGLE);
            brook_straight_rows(root->vfb,root->vfb_stride,image->pixels,image->width,root->vfb_w,root->vfb_h);
            goto publish;
        }
    }
    size_t pixels=(size_t)root->vfb_w*root->vfb_h;
    if (root->composite_pixels<pixels) {
        uint32_t *next=realloc(root->composite,pixels*4);if (!next) return 0;
        root->composite=next;root->composite_pixels=pixels;
    }
    int gpuComposed=0;
#ifdef BROOK_WAYLAND_VULKAN_COMPOSITOR
    gpuComposed=surface_vulkan_compose(root,draws,count,scale);
#endif
    if (!gpuComposed) {
    memset(root->composite,0,pixels*4);
    for (unsigned i=0;i<count;++i) {
        struct brook_draw *d=&draws[i];struct brook_view *view=d->view;
            struct brook_image *image=view->image;const struct brook_geometry *g=&view->geometry;
        if (!image->pixels) return 0; // Never read back a GPU producer as a fallback.
        int64_t dx=d->x*scale,dy=d->y*scale;
        struct brook_geometry physical=*g;physical.w*=scale;physical.h*=scale;
        /* Bit-exact with pixel_over over brook_geometry_pixel samples; see
         * surface-blit.h. The first layer lands on the cleared composite. */
        unsigned flags=(image->opaque?BROOK_BLIT_OPAQUE:0)|(i==0?BROOK_BLIT_DST_CLEAR:0);
        if (!brook_blit_draw(root->composite,root->vfb_w,root->vfb_h,&physical,image->pixels,
                             image->width,image->height,dx,dy,flags)) return 0;
    }
    }
    writeGuard=surface_begin_source_write(root,&writeIncarnation);if(!writeGuard)return 1;
    surface_layer_trace(root,draws,count,gpuComposed?LAYER_PATH_VULKAN:LAYER_PATH_GENERAL);
    brook_straight_rows(root->vfb,root->vfb_stride,root->composite,root->vfb_w,root->vfb_w,root->vfb_h);
publish:;
    int tagged=0;
#ifdef BROOK_WAYLAND_SOURCE_COMMIT_TAG
    /* Tag only when the composed snapshot includes the current root view.
     * This also permits later SHM and subsurface commits to supersede an
     * earlier direct source without guessing from publication generation. */
    if(writeGuard==2 && root->tree.current && root->tree.current->value && root->tree.current->commit){
        int includesRoot=0;
        for(unsigned i=0;i<count;++i)if(draws[i].view==root->tree.current->value){includesRoot=1;break;}
        if(includesRoot && !syscall(BROOK_SYS_WM_PUBLISH_SOURCE_COMMIT,(long)root->wm_id,(long)writeIncarnation,
                                    (long)(root->publish_commit+1),0L,0L,0L)){tagged=1;++root->publish_commit;}
    }
#endif
    if(!tagged)wm_signal_dirty(root->wm_id);
    return 1;
}
static int surface_repaint(struct brook_surface *s) {
    while (s->tree.parent) s=s->tree.parent->user;
    int result=surface_render(s);
    if (!result) wl_client_post_implementation_error(wl_resource_get_client(s->resource),"Brook retained scene failed or exceeded its bounded capacity");
    return result;
}
#endif
