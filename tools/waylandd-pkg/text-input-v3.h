/* Text-input activation/focus for clients such as SDL3. Physical keyboard
 * events remain on wl_keyboard, where clients perform XKB/compose translation.
 * There is no input-method service or on-screen keyboard yet, so do not inject
 * commit_string events or consume physical keys. Keep protocol state per
 * resource, with weak references cleared before seats/surfaces are destroyed. */
struct brook_text_input {
    struct wl_resource *resource;
    struct brook_seat_client *seat;
    struct brook_surface *focus;
    uint32_t commits;
    int enabled, pending_enabled;
    struct brook_text_input *next;
};
static struct brook_text_input *g_text_inputs;

static void text_input_focus(struct brook_seat_client *seat,
                             struct brook_surface *surface) {
    for (struct brook_text_input *t = g_text_inputs; t; t = t->next) {
        if (t->seat != seat || t->focus == surface) continue;
        if (t->focus)
            zwp_text_input_v3_send_leave(t->resource, t->focus->resource);
        t->focus = surface;
        t->enabled = t->pending_enabled = 0;
        if (surface)
            zwp_text_input_v3_send_enter(t->resource, surface->resource);
    }
}

static void text_input_remove_seat(struct brook_seat_client *seat) {
    text_input_focus(seat, NULL);
    for (struct brook_text_input *t = g_text_inputs; t; t = t->next)
        if (t->seat == seat) t->seat = NULL;
}

static void text_input_destroyed(struct wl_resource *resource) {
    struct brook_text_input *t = wl_resource_get_user_data(resource);
    struct brook_text_input **p = &g_text_inputs;
    while (*p && *p != t) p = &(*p)->next;
    if (*p) *p = t->next;
    free(t);
}
static void text_input_destroy(struct wl_client *c, struct wl_resource *r) {
    (void)c; wl_resource_destroy(r);
}
static void text_input_enable(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct brook_text_input *t = wl_resource_get_user_data(r);
    if (t->focus) t->pending_enabled = 1;
}
static void text_input_disable(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct brook_text_input *t = wl_resource_get_user_data(r);
    t->pending_enabled = 0;
}
static void text_input_surrounding(struct wl_client *c, struct wl_resource *r,
                                    const char *text, int32_t cursor, int32_t anchor) {
    (void)c; (void)r; (void)text; (void)cursor; (void)anchor;
}
static void text_input_cause(struct wl_client *c, struct wl_resource *r, uint32_t cause) {
    (void)c; (void)r; (void)cause;
}
static void text_input_content(struct wl_client *c, struct wl_resource *r,
                                uint32_t hint, uint32_t purpose) {
    (void)c; (void)r; (void)hint; (void)purpose;
}
static void text_input_rectangle(struct wl_client *c, struct wl_resource *r,
                                  int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}
static void text_input_commit(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct brook_text_input *t = wl_resource_get_user_data(r);
    ++t->commits;
    t->enabled = t->focus && t->pending_enabled;
    if (t->enabled) {
        /* The first enabled object owns this seat until it disables/leaves. */
        for (struct brook_text_input *other = g_text_inputs; other; other = other->next)
            if (other != t && other->seat == t->seat && other->enabled) {
                t->enabled = t->pending_enabled = 0;
                break;
            }
    }
}
static const struct zwp_text_input_v3_interface text_input_impl = {
    .destroy = text_input_destroy, .enable = text_input_enable,
    .disable = text_input_disable, .set_surrounding_text = text_input_surrounding,
    .set_text_change_cause = text_input_cause, .set_content_type = text_input_content,
    .set_cursor_rectangle = text_input_rectangle, .commit = text_input_commit,
};
static void text_input_get(struct wl_client *c, struct wl_resource *manager,
                            uint32_t id, struct wl_resource *seat_resource) {
    (void)manager;
    struct brook_seat_client *seat = g_seat_clients;
    while (seat && seat->seat != seat_resource) seat = seat->next;
    struct brook_text_input *t = calloc(1, sizeof(*t));
    if (!t) { wl_client_post_no_memory(c); return; }
    t->resource = wl_resource_create(c, &zwp_text_input_v3_interface, 1, id);
    if (!t->resource) { free(t); wl_client_post_no_memory(c); return; }
    t->seat = seat;
    t->next = g_text_inputs; g_text_inputs = t;
    wl_resource_set_implementation(t->resource, &text_input_impl, t, text_input_destroyed);
    if (seat && seat->kb_focus) {
        t->focus = seat->kb_focus;
        zwp_text_input_v3_send_enter(t->resource, t->focus->resource);
    }
}
static const struct zwp_text_input_manager_v3_interface text_input_manager_impl = {
    .destroy = text_input_destroy, .get_text_input = text_input_get,
};
static void text_input_manager_bind(struct wl_client *c, void *data,
                                     uint32_t version, uint32_t id) {
    (void)data; (void)version;
    struct wl_resource *r = wl_resource_create(c, &zwp_text_input_manager_v3_interface, 1, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &text_input_manager_impl, NULL, NULL);
}
