/* Production handlers, real libwayland resources and decoded protocol wire.
 * The lease syscall is controlled here; physical input is a separate VM gate. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
static long test_syscall(long,...);
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall
static uint32_t kernel_token,next_token;
static uint64_t physical_click_serial;
static int revoke_during_release;
static long test_syscall(long number,...) {
    if(number!=BROOK_SYS_WM_POINTER_CAPTURE)return 0;
    va_list ap;va_start(ap,number);long window=va_arg(ap,long),op=va_arg(ap,long);uint32_t token=va_arg(ap,long);va_end(ap);
    assert(window==1);
    if(op==3)return physical_click_serial;
    if(op==0)return kernel_token==token?kernel_token:0;
    if(op==1) {assert(!kernel_token);return kernel_token=++next_token;}
    assert(op==2);
    if(revoke_during_release) {revoke_during_release=0;kernel_token=0;return -116;}
    if(token!=kernel_token)return -116;kernel_token=0;return 0;
}
struct observed {unsigned locked,unlocked,motion,relative,button,frame;int32_t dx,dy;};
static struct observed drain(struct wl_display *d,int fd,uint32_t lock,uint32_t pointer,uint32_t relative) {
    struct observed out={0};uint32_t wire[2048];wl_display_flush_clients(d);
    ssize_t bytes=recv(fd,wire,sizeof(wire),MSG_DONTWAIT);
    if(bytes<0) {assert(errno==EAGAIN);return out;}
    for(size_t off=0;off<(size_t)bytes;) {
        uint32_t *e=(void*)((char*)wire+off);unsigned n=e[1]>>16,op=e[1]&65535;
        assert(n>=8 && !(n&3) && off+n<=(size_t)bytes);
        if(e[0]==lock) {if(op==ZWP_LOCKED_POINTER_V1_LOCKED)++out.locked;else if(op==ZWP_LOCKED_POINTER_V1_UNLOCKED)++out.unlocked;else assert(0);}
        if(e[0]==pointer) {out.motion+=op==WL_POINTER_MOTION;out.button+=op==WL_POINTER_BUTTON;out.frame+=op==WL_POINTER_FRAME;}
        if(e[0]==relative) {
            assert(op==ZWP_RELATIVE_POINTER_V1_RELATIVE_MOTION && n==32);
            ++out.relative;out.dx+=(int32_t)e[4];out.dy+=(int32_t)e[5];
            assert(e[4]==e[6] && e[5]==e[7]);
        }
        off+=n;
    }
    return out;
}
int main(void) {
    int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    seat_bind(client,NULL,5,0);struct brook_seat_client *sc=g_seat_clients;assert(sc && sc->refs==1);
    seat_get_pointer(client,sc->seat,0);struct wl_resource *pointer=sc->pointer;assert(sc->refs==2);
    struct wl_resource *compositor=wl_resource_create(client,&wl_compositor_interface,4,0);assert(compositor);
    compositor_create_surface(client,compositor,0);struct brook_surface *s=g_surfaces;s->wm_id=1;
    struct wl_resource *manager=wl_resource_create(client,&zwp_pointer_constraints_v1_interface,1,0);assert(manager);
    wl_resource_set_implementation(manager,&pointer_constraints_impl,NULL,NULL);
    relative_get_pointer(client,NULL,0,pointer);struct brook_relative_pointer *relative=g_relative_pointers;
    uint32_t relative_id=wl_resource_get_id(relative->resource),pointer_id=wl_resource_get_id(pointer);
    struct pointer_region region={0};struct wl_resource *rr=wl_resource_create(client,&wl_region_interface,1,0);assert(rr);
    wl_resource_set_implementation(rr,&region_impl,&region,NULL);
    region_add(client,rr,0,0,100,100);region_subtract(client,rr,10,10,20,20);
    assert(pointer_region_contains(&region,5,5) && !pointer_region_contains(&region,20,20));
    region_add(client,rr,INT32_MAX-2,0,10,10);assert(pointer_region_contains(&region,INT32_MAX,1));
    surface_set_input_region(client,s->resource,rr);assert(s->input_region.infinite);
    surface_commit(client,s->resource);
    assert(!s->input_region_pending && !s->input_region.infinite);
    region_add(client,rr,10,10,20,20);assert(!pointer_region_contains(&s->input_region,20,20)); // Snapshot, not live alias.
    sc->entered_surface=s;
    struct brook_wm_event click={.type=EVT_MOUSE_BTN_DOWN,.x=5,.y=5};dispatch_surface_event(s,sc,&click,0);
    click.type=EVT_MOUSE_MOVE;click.x=click.y=20;dispatch_surface_event(s,sc,&click,0);
    click.type=EVT_MOUSE_BTN_UP;dispatch_surface_event(s,sc,&click,0);
    struct observed normal=drain(display,sockets[1],0,pointer_id,relative_id);
    assert(normal.button==2 && normal.motion==1); // Input-region hole cannot strand a pressed button.
    sc->pointer_x=sc->pointer_y=20;
    constraint_lock(client,manager,0,s->resource,pointer,NULL,ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    struct brook_pointer_constraint *lock=g_pointer_constraints;uint32_t lock_id=wl_resource_get_id(lock->resource);
    assert(!lock->token && !kernel_token); // Input-region hole blocks activation.
    sc->pointer_x=sc->pointer_y=5;pointer_constraints_refresh();assert(lock->token && kernel_token==lock->token);
    struct observed out=drain(display,sockets[1],lock_id,pointer_id,relative_id);assert(out.locked==1);
    struct brook_wm_event event={.type=EVT_MOUSE_MOVE,.x=8,.y=9};dispatch_surface_event(s,sc,&event,1);
    event=(struct brook_wm_event){.type=EVT_MOUSE_RELATIVE,.x=123,.y=-45,.reserved=kernel_token};dispatch_surface_event(s,sc,&event,1);
    event=(struct brook_wm_event){.type=EVT_MOUSE_BTN_DOWN,.reserved=kernel_token};dispatch_surface_event(s,sc,&event,1);
    out=drain(display,sockets[1],lock_id,pointer_id,relative_id);assert(!out.motion && out.relative==1 && out.button==1 && out.frame==2 && out.dx==123*256 && out.dy==-45*256);
    event=(struct brook_wm_event){.type=EVT_MOUSE_RELATIVE,.x=9,.reserved=kernel_token+1};dispatch_surface_event(s,sc,&event,1);
    event.reserved=0;dispatch_surface_event(s,sc,&event,1);
    out=drain(display,sockets[1],lock_id,pointer_id,relative_id);assert(!out.relative);
    // Release wl_seat before its children. Constraints/relative/pointer remain valid.
    wl_resource_destroy(sc->seat);assert(!sc->seat && g_seat_clients==sc && sc->refs==3);
    uint32_t stale=kernel_token;kernel_token=0;pointer_constraints_refresh();assert(g_pointer_lock_inhibited && !lock->blocked && !lock->token);
    pointer_constraints_refresh();assert(!kernel_token); // Emergency release cannot auto-recapture.
    event.reserved=stale;dispatch_surface_event(s,sc,&event,1);
    out=drain(display,sockets[1],lock_id,pointer_id,relative_id);assert(out.unlocked==1 && !out.relative);
    wl_resource_destroy(lock->resource);
    constraint_lock(client,manager,0,s->resource,pointer,NULL,ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    lock=g_pointer_constraints;assert(!lock->token && g_pointer_lock_inhibited);
    pointer_constraints_refresh();assert(!kernel_token); // Retry/recreated object does not rearm.
    event=(struct brook_wm_event){.type=EVT_MOUSE_BTN_DOWN};dispatch_surface_event(s,sc,&event,2);
    pointer_constraints_refresh();assert(!kernel_token); // Old queued WM button cannot rearm either.
    ++physical_click_serial;pointer_constraints_refresh();assert(lock->token && !g_pointer_lock_inhibited);
    lock_id=wl_resource_get_id(lock->resource);
    constraint_set_region(client,lock->resource,rr);assert(lock->region_pending);
    assert(lock->region.infinite);surface_commit(client,s->resource);assert(!lock->region_pending && !lock->region.infinite);
    pointer_constraints_unmap(s,0);assert(!lock->token && lock->blocked && !kernel_token);
    pointer_constraints_refresh();assert(!kernel_token);
    wl_resource_destroy(lock->resource);
    constraint_confine(client,manager,0,s->resource,pointer,NULL,ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    lock=g_pointer_constraints;assert(lock->confined && !lock->token && !kernel_token);
    wl_resource_destroy(lock->resource);
    constraint_lock(client,manager,0,s->resource,pointer,NULL,ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    lock=g_pointer_constraints;assert(lock->token);
    revoke_during_release=1;wl_resource_destroy(lock->resource);
    assert(g_pointer_lock_inhibited && !kernel_token);
    constraint_lock(client,manager,0,s->resource,pointer,NULL,ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    lock=g_pointer_constraints;assert(!lock->token);
    ++physical_click_serial;pointer_constraints_refresh();assert(lock->token);
    wl_resource_destroy(pointer);assert(!kernel_token && lock->blocked && !lock->pointer && !relative->pointer);
    assert(g_seat_clients==sc && sc->refs==2);
    pointer_constraints_unmap(s,1);assert(!lock->surface);
    wl_resource_destroy(lock->resource);assert(g_seat_clients==sc && sc->refs==1);
    wl_resource_destroy(relative->resource);assert(!g_seat_clients && !g_relative_pointers && !g_pointer_constraints);
    s->wm_id=0;pointer_region_clear(&region);wl_client_destroy(client);close(sockets[1]);wl_display_destroy(display);
    assert(!g_surfaces);
    puts("POINTER_CONSTRAINTS_PASS region snapshots/holes/overflow, lock/relative/button wire, stale epochs, emergency escape, oneshot, inactive confinement, surface/pointer/seat lifetimes");
}
