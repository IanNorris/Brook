/* zwp_idle_inhibit_manager_v1 lifecycle against the production handlers and
 * real libwayland resources; WM_SET_IDLE_INHIBIT (0xB27) is recorded here.
 * Covers create, destroy, a subsurface inhibitor (Firefox's MozContainer),
 * surface unmap and remap, surface destroy (inert inhibitor), several
 * windows, and client disconnect. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
static long test_syscall(long,...);
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall
static unsigned kernel_count[64];
static unsigned set_calls,destroy_calls;
static long refuse;
static long test_syscall(long number,...) {
    va_list ap;va_start(ap,number);long a=va_arg(ap,long),b=va_arg(ap,long);va_end(ap);
    if(number==BROOK_SYS_WM_SET_IDLE_INHIBIT) {
        assert(a>0 && a<64 && b>=0 && b<=(long)BROOK_IDLE_INHIBIT_MAX_PER_WINDOW);
        ++set_calls;if(refuse)return refuse;kernel_count[a]=(unsigned)b;return 0;
    }
    if(number==BROOK_SYS_WM_DESTROY_WINDOW) {
        // The kernel drops the window's inhibitor count with the window.
        assert(a>0 && a<64);++destroy_calls;kernel_count[a]=0;return 0;
    }
    return 0;
}
static struct brook_surface *new_surface(struct wl_client *client,struct wl_resource *compositor) {
    compositor_create_surface(client,compositor,0);return g_surfaces;
}
static struct brook_idle_inhibitor *inhibit(struct wl_client *client,struct wl_resource *manager,struct brook_surface *s) {
    idle_manager_create_inhibitor(client,manager,0,s->resource);return g_idle_inhibitors;
}
int main(void) {
    int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    struct wl_display *display=wl_display_create();assert(display);
    struct wl_client *client=wl_client_create(display,sockets[0]);assert(client);
    struct wl_resource *compositor=wl_resource_create(client,&wl_compositor_interface,4,0);assert(compositor);
    struct wl_resource *manager=wl_resource_create(client,&zwp_idle_inhibit_manager_v1_interface,1,0);assert(manager);
    wl_resource_set_implementation(manager,&idle_manager_impl,NULL,NULL);

    struct brook_surface *top=new_surface(client,compositor);top->wm_id=3;
    // Firefox inhibits on a subsurface of its toplevel.
    struct brook_surface *sub=new_surface(client,compositor);
    sub->role=2;sub->subsurface_parent=top;sub->sibling=top->children;top->children=sub;

    // Create: counted on the root's kernel window at once.
    struct brook_idle_inhibitor *a=inhibit(client,manager,sub);
    assert(a->surface==sub && kernel_count[3]==1 && set_calls==1);
    idle_inhibit_sync();assert(set_calls==1); // unchanged state sends nothing
    struct brook_idle_inhibitor *b=inhibit(client,manager,top);
    assert(kernel_count[3]==2 && set_calls==2);
    // Destroy one: the other still holds.
    wl_resource_destroy(b->resource);assert(kernel_count[3]==1 && set_calls==3);

    // Surface unmapped: the kernel window (and its count) goes; nothing is
    // re-sent for a window that no longer exists.
    unsigned destroys=destroy_calls;
    surface_unmap(top);
    assert(destroy_calls==destroys+1 && !top->wm_id && !top->idle_inhibit_sent && kernel_count[3]==0);
    idle_inhibit_sync();assert(set_calls==3);
    // Remapped, even into the same kernel slot: the live inhibitor is sent again.
    top->wm_id=3;idle_inhibit_sync();assert(kernel_count[3]==1 && set_calls==4);
    surface_unmap(top);top->wm_id=7;
    idle_inhibit_sync();assert(kernel_count[7]==1 && !kernel_count[3]);

    // A second window from the same client counts separately.
    struct brook_surface *other=new_surface(client,compositor);other->wm_id=9;
    struct brook_idle_inhibitor *c=inhibit(client,manager,other);
    assert(kernel_count[9]==1 && kernel_count[7]==1);

    // Surface destroyed before its inhibitor: the inhibitor stays, inert.
    unsigned inert=0;
    wl_resource_destroy(other->resource);
    assert(c->surface==NULL && idle_inhibit_count(&inert)==2 && inert==1 && !kernel_count[9]);
    wl_resource_destroy(c->resource);assert(idle_inhibit_count(&inert)==1 && !inert);

    // Detaching the subsurface from its parent stops it counting there.
    sub->subsurface_parent=NULL;top->children=NULL;idle_inhibit_sync();assert(kernel_count[7]==0);
    sub->subsurface_parent=top;top->children=sub;idle_inhibit_sync();assert(kernel_count[7]==1);

    // A refusing kernel (no 0xB27) is recorded once, not retried each dispatch.
    struct brook_idle_inhibitor *d=inhibit(client,manager,top);(void)d;
    refuse=-38;unsigned calls=set_calls;
    struct brook_idle_inhibitor *e=inhibit(client,manager,top);
    assert(set_calls==calls+1 && g_idle_inhibit_failures==1);
    idle_inhibit_sync();idle_inhibit_sync();assert(set_calls==calls+1);
    refuse=0;calls=set_calls;wl_resource_destroy(e->resource);
    assert(kernel_count[7]==2 && set_calls==calls+1);

    // Client disconnect destroys every inhibitor; the windows go with it.
    (void)a;
    wl_client_destroy(client);
    assert(!g_idle_inhibitors && idle_inhibit_count(NULL)==0);
    for(unsigned i=0;i<64;++i)assert(!kernel_count[i]);
    assert(g_idle_inhibit_created==5 && g_idle_inhibit_destroyed==5);
    wl_display_destroy(display);
    close(sockets[1]);
    printf("idle-inhibit-test: PASS\n");
    return 0;
}
