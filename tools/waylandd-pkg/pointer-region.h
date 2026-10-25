/* Ordered rectangle operations preserve union/subtraction without coordinate
 * overflow or approximating a non-rectangular input region by its bounds. */
struct pointer_rect { int32_t x,y,w,h; int add; };
struct pointer_region { struct pointer_rect *ops; size_t count; int infinite; };
static void pointer_region_clear(struct pointer_region *r) { free(r->ops);memset(r,0,sizeof(*r)); }
static int pointer_region_copy(struct pointer_region *dst,const struct pointer_region *src) {
    struct pointer_region copy={.infinite=src?src->infinite:1};
    if(src && src->count) {
        if(src->count>SIZE_MAX/sizeof(*copy.ops))return 0;
        copy.ops=malloc(src->count*sizeof(*copy.ops));if(!copy.ops)return 0;
        memcpy(copy.ops,src->ops,src->count*sizeof(*copy.ops));copy.count=src->count;
    }
    pointer_region_clear(dst);*dst=copy;return 1;
}
static int pointer_region_contains(const struct pointer_region *r,int32_t x,int32_t y) {
    int inside=r->infinite;
    for(size_t i=0;i<r->count;++i) {
        const struct pointer_rect *p=&r->ops[i];
        if((int64_t)x>=p->x && (int64_t)y>=p->y &&
           (int64_t)x<(int64_t)p->x+p->w && (int64_t)y<(int64_t)p->y+p->h)inside=p->add;
    }
    return inside;
}
static void pointer_region_append(struct wl_client *c,struct wl_resource *r,
                                  int32_t x,int32_t y,int32_t w,int32_t h,int add) {
    struct pointer_region *region=wl_resource_get_user_data(r);
    if(w<=0 || h<=0)return;
    if(region->count>=SIZE_MAX/sizeof(*region->ops)-1) {wl_client_post_no_memory(c);return;}
    void *ops=realloc(region->ops,(region->count+1)*sizeof(*region->ops));
    if(!ops) {wl_client_post_no_memory(c);return;}
    region->ops=ops;region->ops[region->count++]=(struct pointer_rect){x,y,w,h,add};
}
