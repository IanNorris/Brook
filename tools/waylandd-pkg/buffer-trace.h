#ifndef BROOK_BUFFER_TRACE_H
#define BROOK_BUFFER_TRACE_H

/* Single Wayland event loop. No allocation or file writes on the capture
 * path. One capture follows a client PID; resource serials remain distinct
 * even when the client reuses a Wayland object ID or two resources alias. */
#define BUF_TRACE_BUFFERS 512
#define BUF_TRACE_EVENTS 8192

enum buffer_trace_kind {
    BUFFER_ATTACH, BUFFER_COMMIT, BUFFER_OFFER, BUFFER_ACK, BUFFER_SAFE,
    BUFFER_RENDERER_RETIRED, BUFFER_DRAIN, BUFFER_UNCERTAIN, BUFFER_PRODUCER_READY,
    BUFFER_CPU_COPY, BUFFER_RELEASE, BUFFER_TRACE_KINDS,
};
static const char *const buffer_trace_names[] = {
    "attach", "commit", "offer", "renderer_presented", "renderer_safe",
    "renderer_retired", "peer_drain", "uncertain", "producer_ready", "cpu_copy",
    "release_queued",
};

struct buffer_trace_resource {
    struct wl_listener destroy;
    uint64_t serial, device, inode;
    uint32_t object, width, height;
    int live, dmabuf;
};
struct buffer_trace_event {
    uint64_t us, buffer, attachment, cycle, offer, incarnation;
    uint32_t surface, window, uses;
    enum buffer_trace_kind kind;
};
static struct {
    uint64_t start_us, end_us, epoch, next_attachment, events, counts[BUFFER_TRACE_KINDS];
    unsigned buffers, untracked_attaches;
    long pid;
    struct buffer_trace_resource resources[BUF_TRACE_BUFFERS];
    struct buffer_trace_event ring[BUF_TRACE_EVENTS];
} g_buffer_trace;

static void buffer_trace_resource_destroyed(struct wl_listener *listener, void *unused)
{
    (void)unused;
    struct buffer_trace_resource *r = wl_container_of(listener, r, destroy);
    wl_list_remove(&listener->link);
    r->live = 0;
}

static void buffer_trace_start(long pid)
{
    ++g_buffer_trace.epoch;
    g_buffer_trace.start_us = g_now_us();
    g_buffer_trace.end_us = 0;
    g_buffer_trace.pid = pid;
    g_buffer_trace.buffers = g_buffer_trace.untracked_attaches = 0;
    g_buffer_trace.events = 0;
    memset(g_buffer_trace.counts, 0, sizeof(g_buffer_trace.counts));
}

static void buffer_trace_stop(void)
{
    g_buffer_trace.end_us = g_now_us();
    for (unsigned i = 0; i < g_buffer_trace.buffers; ++i) {
        struct buffer_trace_resource *r = &g_buffer_trace.resources[i];
        if (r->live) buffer_trace_resource_destroyed(&r->destroy, NULL);
    }
    g_buffer_trace.pid = 0;
}

static unsigned buffer_trace_resource(struct wl_resource *resource, int dmabuf,
                                      uint64_t device, uint64_t inode, uint32_t width, uint32_t height)
{
    struct wl_listener *listener = wl_resource_get_destroy_listener(resource, buffer_trace_resource_destroyed);
    if (listener) {
        struct buffer_trace_resource *r = wl_container_of(listener, r, destroy);
        return r->serial;
    }
    if (g_buffer_trace.buffers == BUF_TRACE_BUFFERS) {
        ++g_buffer_trace.untracked_attaches;
        return 0;
    }
    unsigned index = g_buffer_trace.buffers++;
    struct buffer_trace_resource *r = &g_buffer_trace.resources[index];
    *r = (struct buffer_trace_resource){
        .destroy.notify = buffer_trace_resource_destroyed, .serial = index + 1,
        .device = device, .inode = inode, .object = wl_resource_get_id(resource),
        .live = 1, .dmabuf = dmabuf, .width = width, .height = height,
    };
    wl_resource_add_destroy_listener(resource, &r->destroy);
    return r->serial;
}

static void buffer_trace_record(enum buffer_trace_kind kind, unsigned buffer,
                                uint64_t attachment, uint64_t cycle, uint32_t surface,
                                uint32_t uses, const struct BrookGpuWindowIdentity *offer)
{
    struct buffer_trace_event *e = &g_buffer_trace.ring[g_buffer_trace.events++ % BUF_TRACE_EVENTS];
    *e = (struct buffer_trace_event){
        .us = g_now_us(), .kind = kind, .buffer = buffer, .attachment = attachment,
        .cycle = cycle, .surface = surface, .uses = uses,
    };
    if (offer) {
        e->window = offer->window;
        e->incarnation = offer->incarnation;
        e->offer = offer->commit;
    }
    ++g_buffer_trace.counts[kind];
}

static int buffer_trace_dump(FILE *f, long pid)
{
    unsigned dmabufs = 0, backings = 0, unknown = 0;
    for (unsigned i = 0; i < g_buffer_trace.buffers; ++i) {
        const struct buffer_trace_resource *r = &g_buffer_trace.resources[i];
        if (!r->dmabuf) continue;
        ++dmabufs;
        if (!r->inode) { ++unknown; continue; }
        unsigned j = 0;
        for (; j < i; ++j)
            if (g_buffer_trace.resources[j].device == r->device &&
                g_buffer_trace.resources[j].inode == r->inode) break;
        if (j == i) ++backings;
    }
    uint64_t dropped = g_buffer_trace.events > BUF_TRACE_EVENTS ? g_buffer_trace.events - BUF_TRACE_EVENTS : 0;
    fprintf(f, "{\"schema\":\"brook-wayland-buffer-trace/1\",\"pid\":%ld,"
               "\"start_us\":%llu,\"end_us\":%llu,\"overwritten_events\":%llu,"
               "\"untracked_attaches\":%u,\"distinct_buffers\":%u,\"distinct_dmabufs\":%u,"
               "\"distinct_known_backings\":%u,\"dmabufs_without_backing\":%u,\"counts\":{",
            pid, (unsigned long long)g_buffer_trace.start_us, (unsigned long long)g_buffer_trace.end_us,
            (unsigned long long)dropped, g_buffer_trace.untracked_attaches,
            g_buffer_trace.buffers, dmabufs, backings, unknown);
    for (unsigned i = 0; i < BUFFER_TRACE_KINDS; ++i)
        fprintf(f, "%s\"%s\":%llu", i ? "," : "", buffer_trace_names[i],
                (unsigned long long)g_buffer_trace.counts[i]);
    fprintf(f, "},\"buffers\":[");
    for (unsigned i = 0; i < g_buffer_trace.buffers; ++i) {
        const struct buffer_trace_resource *r = &g_buffer_trace.resources[i];
        fprintf(f, "%s{\"id\":%u,\"object\":%u,\"kind\":\"%s\",\"width\":%u,\"height\":%u,"
                   "\"device\":%llu,\"inode\":%llu}",
                i ? "," : "", i + 1, r->object, r->dmabuf ? "dmabuf" : "shm", r->width, r->height,
                (unsigned long long)r->device, (unsigned long long)r->inode);
    }
    fprintf(f, "],\"events\":[");
    for (uint64_t i = dropped; i < g_buffer_trace.events; ++i) {
        const struct buffer_trace_event *e = &g_buffer_trace.ring[i % BUF_TRACE_EVENTS];
        fprintf(f, "%s{\"us\":%llu,\"event\":\"%s\",\"buffer\":%llu,\"attach\":%llu,"
                   "\"cycle\":%llu,\"surface\":%u,\"uses\":%u,\"window\":%u,"
                   "\"incarnation\":%llu,\"offer\":%llu}",
                i != dropped ? "," : "", (unsigned long long)e->us, buffer_trace_names[e->kind],
                (unsigned long long)e->buffer, (unsigned long long)e->attachment,
                (unsigned long long)e->cycle, e->surface, e->uses, e->window,
                (unsigned long long)e->incarnation, (unsigned long long)e->offer);
    }
    fputs("]}\n", f);
    return !ferror(f);
}

/* Request is a regular file containing the exact Wayland-client PID. Keep it
 * present for the sample; removing it freezes and atomically publishes JSON.
 * Poll with the existing status timer, outside the measured frame hot path. */
static void buffer_trace_poll(const char *status_path)
{
    char request[304], path[304], tmp[320];
    if (snprintf(request, sizeof(request), "%s.buffer-trace.request", status_path) >= (int)sizeof(request) ||
        snprintf(path, sizeof(path), "%s.buffer-trace.json", status_path) >= (int)sizeof(path)) return;
    int fd = open(request, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        if (!g_buffer_trace.pid) {
            struct stat st;
            char value[32];
            ssize_t n = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? read(fd, value, sizeof(value) - 1) : -1;
            if (n > 0 && value[0] >= '1' && value[0] <= '9') {
                value[n] = 0;
                char *end;
                errno = 0;
                long pid = strtol(value, &end, 10);
                if (!errno && pid > 0 && pid <= INT_MAX &&
                    (!*end || (*end == '\n' && !end[1])) && end - value + (*end == '\n') == n)
                    buffer_trace_start(pid);
            }
        }
        close(fd);
        return;
    }
    if (errno != ENOENT || !g_buffer_trace.pid) return;
    const long pid = g_buffer_trace.pid;
    buffer_trace_stop();
    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
    fd = mkstemp(tmp);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return; }
    const int written = buffer_trace_dump(f, pid);
    if (fclose(f) || !written || rename(tmp, path)) unlink(tmp);
}

#endif
