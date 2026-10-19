#ifndef BROOK_SURFACE_SCENE_H
#define BROOK_SURFACE_SCENE_H
/* Single-threaded Wayland adapter. Pixel contents are immutable. The optional
 * GPU storage cache can migrate only after a verified clone; scene snapshots
 * retain logical images and never retain producer resource numbers alone. */
struct brook_scene_owner { unsigned refs;int fd; };
struct brook_image {
    unsigned refs;
    uint32_t width,height,format;
    uint32_t *pixels;
    struct brook_buffer *source;
    struct brook_scene_owner *owner;
    uint64_t version;
    int uncertain;
};
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
    free(image->pixels);free(image);
}
static struct brook_image *image_snapshot(const uint8_t *src,int w,int h,int stride,uint32_t format) {
    if (!src || w<=0 || h<=0 || w>16384 || h>16384 || stride<0 ||
        (uint64_t)stride<(uint64_t)w*4 || (format!=WL_SHM_FORMAT_ARGB8888 && format!=WL_SHM_FORMAT_XRGB8888)) return NULL;
    struct brook_image *image=calloc(1,sizeof(*image));if (!image) return NULL;
    image->pixels=malloc((size_t)w*h*4);
    if (!image->pixels) { free(image);return NULL; }
    image->refs=1;image->width=w;image->height=h;image->format=format==WL_SHM_FORMAT_ARGB8888?1:2;
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
};
static void view_ref(void *value) { if(value) ++((struct brook_view *)value)->refs; }
static void view_unref(void *value) {
    struct brook_view *view=value;
    if (!view || --view->refs) return;
    image_unref(view->image);free(view);
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
    }
    struct brook_frame_callback *frame;
    uint32_t now=g_trace_frames?g_now_ms():0;
    wl_list_for_each(frame,&s->pending_frames,link) {
        frame->commit=s->tree.sequence+1;frame->committed_ms=now;
    }
    wl_list_insert_list(s->waiting_frames.prev,&s->pending_frames);wl_list_init(&s->pending_frames);
    int result=bt_commit(&s->tree,view);view_unref(view);return result;
}
static void surface_unmap(struct brook_surface *s) {
    if (s->wm_id && (s->xdg_toplevel || s->xdg_popup)) {
        s->xdg_initial_commit_done=0;s->xdg_acked=0;
    }
    s->toplevel_maximized=s->toplevel_resizing=0;s->configure_width=s->configure_height=0;s->configure_size_pending=s->configure_size_acked=0;s->configure_size_serial=0;
    scene_owner_unref(s->scene);s->scene=NULL;s->scene_disabled=0;
    if (s->wm_id) wm_destroy_window(s->wm_id);
    s->wm_id=0;s->vfb=NULL;s->vfb_w=s->vfb_h=s->vfb_stride=0;
}
static int surface_window(struct brook_surface *s,struct brook_view *view) {
    if (!view) { if (s->xdg_toplevel || s->xdg_popup) surface_unmap(s);return 1; }
    if (!(s->xdg_toplevel || s->xdg_popup)) return 1;
    if (!s->xdg_acked) return 1;
    struct brook_wm_create_out out={0};
    if (!s->wm_id) {
        int csd=s->xdg_popup || s->deco_csd || (s->xdg_toplevel && !s->deco_negotiated);
        unsigned flags=(csd?BROOK_WM_CREATE_FLAG_CSD:0)|(s->xdg_popup?BROOK_WM_CREATE_FLAG_NO_FOCUS:0);
        if (wm_create_window(view->geometry.w,view->geometry.h,s->title[0]?s->title:NULL,flags,&out) || !out.wm_id) return 0;
        s->wm_id=out.wm_id;s->scene_disabled=0;
        wm_register_client(s->wm_id,s->resource);
        if (s->app_id[0]) wm_set_app_id(s->wm_id,s->app_id);
        s->deco_pending=0;popup_apply_position(s);
    } else if (s->vfb_w!=view->geometry.w || s->vfb_h!=view->geometry.h) {
        if (wm_resize_vfb(s->wm_id,view->geometry.w,view->geometry.h,&out) || !out.vfb_user) return 0;
    } else return 1;
    s->vfb=(uint32_t *)(uintptr_t)out.vfb_user;s->vfb_stride=out.vfb_stride;
    s->vfb_w=view->geometry.w;s->vfb_h=view->geometry.h;return 1;
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
static uint32_t pixel_over(uint32_t src, uint32_t dst) {
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
static uint32_t pixel_straight(uint32_t px) {
    unsigned a = px >> 24;
    if (!a || a == 255) return a ? px : 0;
    uint32_t out = px & 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned v = (((px >> shift) & 255) * 255 + a / 2) / a;
        out |= (v > 255 ? 255 : v) << shift;
    }
    return out;
}
static int surface_render(struct brook_surface *root) {
    if ((root->xdg_toplevel || root->xdg_popup) && !root->xdg_acked) return 1;
    struct brook_view *base=root->tree.current?root->tree.current->value:NULL;
    if (!surface_window(root,base)) return 0;
    if (!root->wm_id || !root->vfb) return 1;
    if (!scene_ensure(root)) return 0;
    struct brook_draw draws[64];unsigned count=0;
    if (!surface_layers(root,draws,&count)) return 0;
    if (root->scene) {
        struct SceneLayerDesc layers[64];unsigned n=0;
        for (unsigned i=0;i<count;++i) {
            struct brook_draw *d=&draws[i];struct brook_view *view=d->view;
            struct brook_image *image=view->image;const struct brook_geometry *g=&view->geometry;
            if (d->x>=root->vfb_w || d->y>=root->vfb_h || d->x+g->w<=0 || d->y+g->h<=0) continue;
            if (!image_gpu(image,root->scene)) return 0;
            layers[n++]=(struct SceneLayerDesc){image->version,g->sx,g->sy,g->sw,g->sh,(int32_t)d->x,(int32_t)d->y,g->w,g->h,255,0,0,root->vfb_w,root->vfb_h,SCENE_LAYER_FIXED8|(g->transform<<SCENE_LAYER_TRANSFORM_SHIFT)};
        }
        struct ScenePresentDesc desc={(uintptr_t)layers,n,root->vfb_w,root->vfb_h,0};
        long result=scene_ctl(root->scene->fd,SCENE_CTL_PRESENT,&desc,sizeof(desc));
        // Native resize/content generation changes can safely discard a frame.
        // Keep the captured versions; the next commit/configure can redraw.
        return result==0 || result==-116;
    }
    size_t pixels=(size_t)root->vfb_w*root->vfb_h;
    if (root->composite_pixels<pixels) {
        uint32_t *next=realloc(root->composite,pixels*4);if (!next) return 0;
        root->composite=next;root->composite_pixels=pixels;
    }
    memset(root->composite,0,pixels*4);
    for (unsigned i=0;i<count;++i) {
        struct brook_draw *d=&draws[i];struct brook_view *view=d->view;
            struct brook_image *image=view->image;const struct brook_geometry *g=&view->geometry;
        if (!image->pixels) return 0; // Never read back a GPU producer as a fallback.
        int64_t x0=d->x<0?-d->x:0,y0=d->y<0?-d->y:0,x1=g->w,y1=g->h;
        if (d->x+x1>root->vfb_w) x1=(int64_t)root->vfb_w-d->x;
        if (d->y+y1>root->vfb_h) y1=(int64_t)root->vfb_h-d->y;
        for (int64_t y=y0;y<y1;++y) for (int64_t x=x0;x<x1;++x) {
            uint32_t *out=&root->composite[(size_t)(d->y+y)*root->vfb_w+d->x+x];
            uint32_t px,py;brook_geometry_pixel(g,image->width,image->height,x,y,&px,&py);
            *out=pixel_over(image->pixels[(size_t)py*image->width+px],*out);
        }
    }
    for (unsigned y=0;y<root->vfb_h;++y) for (unsigned x=0;x<root->vfb_w;++x)
        root->vfb[(size_t)y*root->vfb_stride+x]=pixel_straight(root->composite[(size_t)y*root->vfb_w+x]);
    wm_signal_dirty(root->wm_id);return 1;
}
static int surface_repaint(struct brook_surface *s) {
    while (s->tree.parent) s=s->tree.parent->user;
    int result=surface_render(s);
    if (!result) wl_client_post_implementation_error(wl_resource_get_client(s->resource),"Brook retained scene failed or exceeded its bounded capacity");
    return result;
}
#endif
