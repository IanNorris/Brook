/* One physical pointer seat. Protocol resources retain their seat independently
 * of wl_seat lifetime; surface and pointer destruction detach them explicitly. */
struct brook_relative_pointer {
    struct wl_resource *resource,*pointer;
    struct brook_seat_client *seat;
    struct brook_relative_pointer *next;
};
struct brook_pointer_constraint {
    struct wl_resource *resource,*pointer;
    struct brook_surface *surface;
    struct brook_seat_client *seat;
    struct pointer_region region,pending_region;
    int region_pending,confined,blocked,oneshot;
    uint32_t token;
    struct brook_pointer_constraint *next;
};
static struct brook_relative_pointer *g_relative_pointers;
static struct brook_pointer_constraint *g_pointer_constraints;
static int g_pointer_lock_inhibited;
static uint64_t g_pointer_lock_rearm_serial;
static long pointer_lease(struct brook_surface *s,int op,uint32_t token) {
    return syscall(BROOK_SYS_WM_POINTER_CAPTURE,(long)s->wm_id,(long)op,(long)token,0L,0L);
}
static int pointer_surface_locked(struct brook_surface *s) {
    for(struct brook_pointer_constraint *p=g_pointer_constraints;p;p=p->next)
        if(p->surface==s && p->token)return 1;
    return 0;
}
static int pointer_event_token(struct brook_surface *s,uint32_t token) {
    for(struct brook_pointer_constraint *p=g_pointer_constraints;p;p=p->next)
        if(p->surface==s && p->token==token)return 1;
    return 0;
}
static void pointer_lock_inhibit(struct brook_surface *s) {
    long serial=pointer_lease(s,3,0);
    g_pointer_lock_inhibited=1;
    g_pointer_lock_rearm_serial=serial>=0?(uint64_t)serial:UINT64_MAX;
}
static void pointer_constraint_unlock(struct brook_pointer_constraint *p,int notify,int block) {
    if(p->token) {
        if(p->surface && p->surface->wm_id) {
            if(pointer_lease(p->surface,0,p->token)!=(long)p->token) {
                pointer_lock_inhibit(p->surface);
            }
            if(pointer_lease(p->surface,2,p->token)<0)pointer_lock_inhibit(p->surface);
        }
        p->token=0;
        if(notify)zwp_locked_pointer_v1_send_unlocked(p->resource);
        if(p->oneshot)p->blocked=1;
    }
    if(block)p->blocked=1;
}
static void pointer_constraints_refresh(void) {
    for(struct brook_pointer_constraint *p=g_pointer_constraints;p;p=p->next) {
        if(p->token && pointer_lease(p->surface,0,p->token)!=(long)p->token) {
            // Includes emergency release. Inhibition belongs to the physical
            // seat, surviving client destroy/recreate until a fresh real click.
            pointer_constraint_unlock(p,1,0);
        }
        if(p->confined || p->blocked || !p->surface || !p->pointer)continue;
        struct brook_surface *s=p->surface;struct brook_seat_client *sc=p->seat;
        if(g_pointer_lock_inhibited && s->wm_id) {
            long serial=pointer_lease(s,3,0);
            if(serial>=0 && (uint64_t)serial>g_pointer_lock_rearm_serial)g_pointer_lock_inhibited=0;
        }
        int eligible=!g_pointer_lock_inhibited && s->wm_id && sc->entered_surface==s &&
            pointer_region_contains(&s->input_region,sc->pointer_x,sc->pointer_y) &&
            pointer_region_contains(&p->region,sc->pointer_x,sc->pointer_y);
        if(p->token && !eligible)pointer_constraint_unlock(p,1,0);
        if(!p->token && !p->blocked && eligible) {
            long token=pointer_lease(s,1,0);
            if(token>0 && (unsigned long)token<=UINT32_MAX) {
                p->token=token;zwp_locked_pointer_v1_send_locked(p->resource);
            }
        }
    }
}
static void pointer_constraints_unmap(struct brook_surface *s,int destroy) {
    for(struct brook_pointer_constraint *p=g_pointer_constraints;p;p=p->next)if(p->surface==s) {
        pointer_constraint_unlock(p,1,destroy);
        if(destroy)p->surface=NULL;
    }
}
static void pointer_constraints_commit(struct brook_surface *s) {
    for(struct brook_pointer_constraint *p=g_pointer_constraints;p;p=p->next)if(p->surface==s && p->region_pending) {
        pointer_region_clear(&p->region);p->region=p->pending_region;
        memset(&p->pending_region,0,sizeof(p->pending_region));p->region_pending=0;
    }
    pointer_constraints_refresh();
}
static void pointer_relative_dispatch(struct brook_surface *s,struct brook_seat_client *sc,
                                      const struct brook_wm_event *event) {
    if(!sc || sc->entered_surface!=s || (event->reserved && !pointer_event_token(s,event->reserved)))return;
    // A pre-lock ordinary raw packet must not cross into a later lease either.
    if(!event->reserved && pointer_surface_locked(s))return;
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now)<0)return;
    uint64_t us=(uint64_t)now.tv_sec*1000000u+(uint64_t)now.tv_nsec/1000u;
    for(struct brook_relative_pointer *p=g_relative_pointers;p;p=p->next)if(p->seat==sc && p->pointer) {
        zwp_relative_pointer_v1_send_relative_motion(p->resource,us>>32,(uint32_t)us,
            wl_fixed_from_int(event->x),wl_fixed_from_int(event->y),
            wl_fixed_from_int(event->x),wl_fixed_from_int(event->y));
        // SDL batches its pending motion until wl_pointer.frame. A relative
        // event without this boundary can be overwritten by the next packet.
        if(wl_resource_get_version(p->pointer)>=5)wl_pointer_send_frame(p->pointer);
    }
}
static void pointer_object_destroy_req(struct wl_client *c,struct wl_resource *r) { (void)c;wl_resource_destroy(r); }
static void relative_resource_destroy(struct wl_resource *r) {
    struct brook_relative_pointer *p=wl_resource_get_user_data(r);
    struct brook_relative_pointer **it=&g_relative_pointers;while(*it && *it!=p)it=&(*it)->next;
    if(*it)*it=p->next;
    seat_unref(p->seat);free(p);
}
static const struct zwp_relative_pointer_v1_interface relative_pointer_impl={.destroy=pointer_object_destroy_req};
static void relative_get_pointer(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *pointer) {
    (void)r;
    if(!wl_resource_instance_of(pointer,&wl_pointer_interface,&pointer_impl)) {
        wl_client_post_implementation_error(c,"foreign wl_pointer");return;
    }
    struct brook_seat_client *seat=seat_for_resource(pointer);
    struct brook_relative_pointer *p=calloc(1,sizeof(*p));
    if(!p) {wl_client_post_no_memory(c);return;}
    p->resource=wl_resource_create(c,&zwp_relative_pointer_v1_interface,1,id);
    if(!p->resource) {free(p);wl_client_post_no_memory(c);return;}
    p->pointer=pointer;p->seat=seat;++seat->refs;
    p->next=g_relative_pointers;g_relative_pointers=p;
    wl_resource_set_implementation(p->resource,&relative_pointer_impl,p,relative_resource_destroy);
}
static const struct zwp_relative_pointer_manager_v1_interface relative_manager_impl={
    .destroy=pointer_object_destroy_req,.get_relative_pointer=relative_get_pointer};
static void relative_manager_bind(struct wl_client *c,void *data,uint32_t version,uint32_t id) {
    (void)data;(void)version;struct wl_resource *r=wl_resource_create(c,&zwp_relative_pointer_manager_v1_interface,1,id);
    if(!r) {wl_client_post_no_memory(c);return;}
    wl_resource_set_implementation(r,&relative_manager_impl,NULL,NULL);
}
static void constraint_resource_destroy(struct wl_resource *r) {
    struct brook_pointer_constraint *p=wl_resource_get_user_data(r);
    pointer_constraint_unlock(p,0,1);
    struct brook_pointer_constraint **it=&g_pointer_constraints;while(*it && *it!=p)it=&(*it)->next;
    if(*it)*it=p->next;
    pointer_region_clear(&p->region);pointer_region_clear(&p->pending_region);
    seat_unref(p->seat);free(p);
}
static void constraint_set_region(struct wl_client *c,struct wl_resource *r,struct wl_resource *region) {
    struct brook_pointer_constraint *p=wl_resource_get_user_data(r);
    if(!pointer_region_copy(&p->pending_region,region?wl_resource_get_user_data(region):NULL)) {
        wl_client_post_no_memory(c);return;
    }
    p->region_pending=1;
}
static void constraint_cursor_hint(struct wl_client *c,struct wl_resource *r,wl_fixed_t x,wl_fixed_t y) {
    // Warping to the optional hint is compositor policy. Kernel release restores
    // the acquisition position instead; accepting the hint does not warp.
    (void)c;(void)r;(void)x;(void)y;
}
static const struct zwp_locked_pointer_v1_interface locked_pointer_impl={
    .destroy=pointer_object_destroy_req,.set_cursor_position_hint=constraint_cursor_hint,.set_region=constraint_set_region};
static const struct zwp_confined_pointer_v1_interface confined_pointer_impl={
    .destroy=pointer_object_destroy_req,.set_region=constraint_set_region};
static void constraint_create(struct wl_client *c,struct wl_resource *r,uint32_t id,
    struct wl_resource *surface,struct wl_resource *pointer,struct wl_resource *region,uint32_t lifetime,int confined) {
    if(!wl_resource_instance_of(surface,&wl_surface_interface,&surface_impl) ||
       !wl_resource_instance_of(pointer,&wl_pointer_interface,&pointer_impl) ||
       (lifetime!=ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT && lifetime!=ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT)) {
        wl_client_post_implementation_error(c,"invalid pointer constraint");return;
    }
    struct brook_surface *s=wl_resource_get_user_data(surface);
    for(struct brook_pointer_constraint *p=g_pointer_constraints;p;p=p->next)if(p->surface==s) {
        wl_resource_post_error(r,ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,"surface already constrained on brook-seat");return;
    }
    struct brook_pointer_constraint *p=calloc(1,sizeof(*p));
    if(!p) {wl_client_post_no_memory(c);return;}
    if(!pointer_region_copy(&p->region,region?wl_resource_get_user_data(region):NULL)) {free(p);wl_client_post_no_memory(c);return;}
    p->resource=wl_resource_create(c,confined?&zwp_confined_pointer_v1_interface:&zwp_locked_pointer_v1_interface,1,id);
    if(!p->resource) {pointer_region_clear(&p->region);free(p);wl_client_post_no_memory(c);return;}
    p->surface=s;p->pointer=pointer;p->seat=seat_for_resource(pointer);++p->seat->refs;
    p->confined=confined;p->oneshot=lifetime==ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT;
    p->next=g_pointer_constraints;g_pointer_constraints=p;
    wl_resource_set_implementation(p->resource,confined?(const void*)&confined_pointer_impl:(const void*)&locked_pointer_impl,p,constraint_resource_destroy);
    // Confinement deliberately remains inactive until kernel region clamping
    // exists; the protocol permits requests whose activation policy is unmet.
    pointer_constraints_refresh();
}
static void constraint_lock(struct wl_client *c,struct wl_resource *r,uint32_t id,
    struct wl_resource *s,struct wl_resource *p,struct wl_resource *region,uint32_t lifetime) {
    constraint_create(c,r,id,s,p,region,lifetime,0);
}
static void constraint_confine(struct wl_client *c,struct wl_resource *r,uint32_t id,
    struct wl_resource *s,struct wl_resource *p,struct wl_resource *region,uint32_t lifetime) {
    constraint_create(c,r,id,s,p,region,lifetime,1);
}
static const struct zwp_pointer_constraints_v1_interface pointer_constraints_impl={
    .destroy=pointer_object_destroy_req,.lock_pointer=constraint_lock,.confine_pointer=constraint_confine};
static void pointer_constraints_bind(struct wl_client *c,void *data,uint32_t version,uint32_t id) {
    (void)data;(void)version;struct wl_resource *r=wl_resource_create(c,&zwp_pointer_constraints_v1_interface,1,id);
    if(!r) {wl_client_post_no_memory(c);return;}
    wl_resource_set_implementation(r,&pointer_constraints_impl,NULL,NULL);
}
static void pointer_objects_remove_pointer(struct wl_resource *r) {
    for(struct brook_pointer_constraint *p=g_pointer_constraints;p;p=p->next)if(p->pointer==r) {
        pointer_constraint_unlock(p,1,1);p->pointer=NULL;
    }
    for(struct brook_relative_pointer *p=g_relative_pointers;p;p=p->next)if(p->pointer==r)p->pointer=NULL;
}
