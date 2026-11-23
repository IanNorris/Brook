/* zwp_idle_inhibit_manager_v1 (idle-inhibit-unstable-v1).
 *
 * Browsers inhibit on a surface while video plays (Firefox: its MozContainer
 * subsurface; Chromium: its toplevel). waylandd resolves each inhibitor to the
 * kernel window of its surface tree (subsurfaces walk to their root) and
 * relays a per-window count with WM_SET_IDLE_INHIBIT (0xB27). The kernel
 * applies a window's count only while the window is shown and not minimised,
 * which is the protocol's "visible" condition, and holds the display idle
 * timeout off meanwhile.
 *
 * Lifecycle: destroying the inhibitor (or the client disconnecting, which
 * destroys it) removes it. A destroyed surface leaves its inhibitor inert, as
 * the protocol requires. Unmapping a surface destroys its kernel window, and
 * the kernel drops that window's count with it, so surface_unmap() only
 * forgets what was sent; a remap sends the count again. */
#define BROOK_SYS_WM_SET_IDLE_INHIBIT 0xB27
#define BROOK_IDLE_INHIBIT_MAX_PER_WINDOW 1024u
struct brook_idle_inhibitor {
    struct wl_resource *resource;
    struct brook_surface *surface;
    struct brook_idle_inhibitor *next;
};
static struct brook_idle_inhibitor *g_idle_inhibitors;
static uint64_t g_idle_inhibit_created, g_idle_inhibit_destroyed, g_idle_inhibit_failures;
static struct brook_surface *idle_inhibit_root(struct brook_surface *s) {
    if (!s || s->is_cursor) return NULL;
    while (s->subsurface_parent) s = s->subsurface_parent;
    return s->wm_id ? s : NULL;
}
static void idle_inhibit_sync(void) {
    for (struct brook_surface *s = g_surfaces; s; s = s->next) s->idle_inhibit_want = 0;
    for (struct brook_idle_inhibitor *p = g_idle_inhibitors; p; p = p->next) {
        struct brook_surface *root = idle_inhibit_root(p->surface);
        if (root && root->idle_inhibit_want < BROOK_IDLE_INHIBIT_MAX_PER_WINDOW) ++root->idle_inhibit_want;
    }
    for (struct brook_surface *s = g_surfaces; s; s = s->next) {
        if (!s->wm_id || s->idle_inhibit_want == s->idle_inhibit_sent) continue;
        long rc = syscall(BROOK_SYS_WM_SET_IDLE_INHIBIT, (long)s->wm_id, (long)s->idle_inhibit_want);
        fprintf(stderr, "[waylandd] idle inhibit wm=%u app_id=%s count=%u->%u rc=%ld\n",
                s->wm_id, s->app_id[0] ? s->app_id : "-", s->idle_inhibit_sent, s->idle_inhibit_want, rc);
        /* A refusal (an older kernel without 0xB27) is recorded once per
         * change rather than retried every dispatch. */
        if (rc) ++g_idle_inhibit_failures;
        s->idle_inhibit_sent = s->idle_inhibit_want;
    }
}
static unsigned idle_inhibit_count(unsigned *inert) {
    unsigned n = 0, dead = 0;
    for (struct brook_idle_inhibitor *p = g_idle_inhibitors; p; p = p->next) { ++n; dead += !p->surface; }
    if (inert) *inert = dead;
    return n;
}
/* Status-file string: app_ids are client-chosen, so keep only plain bytes. */
static void idle_inhibit_json_text(FILE *f, const char *text) {
    fputc('"', f);
    for (const unsigned char *c = (const unsigned char *)text; c && *c; ++c)
        fputc(*c < 0x20 || *c == '"' || *c == '\\' || *c >= 0x7f ? '?' : *c, f);
    fputc('"', f);
}
static void idle_inhibitor_resource_destroy(struct wl_resource *r) {
    struct brook_idle_inhibitor *p = wl_resource_get_user_data(r);
    if (!p) return;
    for (struct brook_idle_inhibitor **pp = &g_idle_inhibitors; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    ++g_idle_inhibit_destroyed;
    free(p);
    idle_inhibit_sync();
}
static void idle_inhibitor_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct zwp_idle_inhibitor_v1_interface idle_inhibitor_impl = { .destroy = idle_inhibitor_destroy };
static void idle_inhibit_surface_destroyed(struct brook_surface *s) {
    for (struct brook_idle_inhibitor *p = g_idle_inhibitors; p; p = p->next)
        if (p->surface == s) p->surface = NULL;
}
static void idle_manager_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void idle_manager_create_inhibitor(struct wl_client *c, struct wl_resource *manager, uint32_t id,
                                          struct wl_resource *surface) {
    struct brook_surface *s = wl_resource_get_user_data(surface);
    struct brook_idle_inhibitor *p = calloc(1, sizeof(*p));
    struct wl_resource *r = p ? wl_resource_create(c, &zwp_idle_inhibitor_v1_interface,
                                                   wl_resource_get_version(manager), id) : NULL;
    if (!r) { free(p); wl_client_post_no_memory(c); return; }
    p->resource = r; p->surface = s; p->next = g_idle_inhibitors; g_idle_inhibitors = p;
    wl_resource_set_implementation(r, &idle_inhibitor_impl, p, idle_inhibitor_resource_destroy);
    ++g_idle_inhibit_created;
    pid_t pid = 0; wl_client_get_credentials(c, &pid, NULL, NULL);
    struct brook_surface *root = idle_inhibit_root(s);
    fprintf(stderr, "[waylandd] idle inhibitor created pid=%ld surface=%u root_wm=%u\n",
            (long)pid, wl_resource_get_id(surface), root ? root->wm_id : 0);
    idle_inhibit_sync();
}
static const struct zwp_idle_inhibit_manager_v1_interface idle_manager_impl = {
    .destroy = idle_manager_destroy,
    .create_inhibitor = idle_manager_create_inhibitor,
};
static void idle_manager_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_idle_inhibit_manager_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &idle_manager_impl, NULL, NULL);
}
