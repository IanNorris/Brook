/*
 * brook-player.c — Minimal video player for Brook OS.
 *
 * Uses ffmpeg libav* for decoding and Wayland wl_shm for display.
 * Audio output via direct /dev/dsp writes (OSS, S16_LE 44100 stereo).
 * Bypasses SDL entirely — renders decoded frames directly to wl_shm
 * buffers via xdg_toplevel.
 *
 * Usage: brook-player [-fs] [-autoexit] <video-file>
 *
 * Design:
 *   - Single-threaded decode + display loop
 *   - Double-buffered wl_shm: draw into back buffer while front is
 *     displayed, swap on wl_buffer.release
 *   - Frame pacing via nanosleep based on video PTS
 *   - Audio: decoded → resampled to S16_LE/44100/2ch → /dev/dsp
 *   - Fullscreen via xdg_toplevel.set_fullscreen
 *   - Exits on video end (always, for now)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <poll.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif

static struct wl_display    *g_dpy   = NULL;
static struct wl_registry   *g_reg   = NULL;
static struct wl_shm        *g_shm   = NULL;
static struct wl_compositor *g_comp  = NULL;
static struct xdg_wm_base   *g_wm   = NULL;
static struct wl_surface    *g_surf  = NULL;
static struct xdg_surface   *g_xsurf = NULL;
static struct xdg_toplevel  *g_tl    = NULL;

static int g_configured   = 0;
static int g_close_req    = 0;
static int g_width        = 0;   /* configured window size (0 = use video) */
static int g_height       = 0;

/* Double-buffered wl_shm state */
typedef struct {
    struct wl_buffer *wl_buf;
    uint8_t          *pixels;   /* mmap'd shm region */
    int               fd;
    size_t            size;
    int               busy;     /* 1 while server holds this buffer */
} ShmBuffer;

static ShmBuffer g_bufs[2];
static int       g_back = 0;   /* index of the back buffer to draw into */

/* Video state */
static int g_vid_w = 0;
static int g_vid_h = 0;
static int g_fullscreen = 0;

/* Audio state */
static int g_dsp_fd = -1;              /* /dev/dsp file descriptor */
static struct SwrContext *g_swr = NULL; /* audio resampler */
static AVCodecContext *g_audDecCtx = NULL;
static int g_audIdx = -1;              /* audio stream index */
static AVStream *g_audStream = NULL;

/* Audio output parameters (Brook's /dev/dsp default) */
#define AUDIO_RATE     44100
#define AUDIO_CHANNELS 2
#define AUDIO_FORMAT   AV_SAMPLE_FMT_S16

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int memfd_create_shim(const char *name, unsigned int flags)
{
    return (int)syscall(319, name, flags);
}

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ */
/* SHM buffer management                                               */
/* ------------------------------------------------------------------ */

static void on_buf_release(void *data, struct wl_buffer *wl_buf)
{
    ShmBuffer *b = data;
    (void)wl_buf;
    b->busy = 0;
}
static const struct wl_buffer_listener buf_lis = { .release = on_buf_release };

static int shm_buf_init(ShmBuffer *b, int w, int h)
{
    int stride = w * 4;
    size_t sz  = (size_t)stride * h;

    int fd = memfd_create_shim("brook-player", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, sz) < 0) { close(fd); return -1; }

    uint8_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { close(fd); return -1; }

    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, sz);
    b->wl_buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                           WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    wl_buffer_add_listener(b->wl_buf, &buf_lis, b);

    b->pixels = px;
    b->fd     = fd;
    b->size   = sz;
    b->busy   = 0;
    return 0;
}

static void shm_buf_destroy(ShmBuffer *b)
{
    if (b->wl_buf) wl_buffer_destroy(b->wl_buf);
    if (b->pixels) munmap(b->pixels, b->size);
    if (b->fd >= 0) close(b->fd);
    memset(b, 0, sizeof(*b));
    b->fd = -1;
}

static int shm_bufs_create(int w, int h)
{
    for (int i = 0; i < 2; i++) {
        if (g_bufs[i].wl_buf) shm_buf_destroy(&g_bufs[i]);
        if (shm_buf_init(&g_bufs[i], w, h) < 0) return -1;
    }
    g_back = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wayland listeners                                                   */
/* ------------------------------------------------------------------ */

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)data;
    if (!strcmp(iface, "wl_shm"))
        g_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_compositor"))
        g_comp = wl_registry_bind(reg, name, &wl_compositor_interface,
                                   version < 4 ? version : 4);
    else if (!strcmp(iface, "xdg_wm_base"))
        g_wm = wl_registry_bind(reg, name, &xdg_wm_base_interface,
                                 version < 3 ? version : 3);
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n)
{
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_lis = {
    .global = on_global, .global_remove = on_global_remove,
};

static void on_wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_lis = { .ping = on_wm_ping };

static void on_xdg_surface_configure(void *data, struct xdg_surface *xs,
                                        uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xs, serial);
    g_configured = 1;
}
static const struct xdg_surface_listener xs_lis = {
    .configure = on_xdg_surface_configure,
};

static void on_toplevel_configure(void *data, struct xdg_toplevel *t,
                                     int32_t w, int32_t h,
                                     struct wl_array *states)
{
    (void)data; (void)t; (void)states;
    if (w > 0 && h > 0) {
        g_width  = w;
        g_height = h;
    }
}
static void on_toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)data; (void)t;
    g_close_req = 1;
}
static void on_toplevel_configure_bounds(void *data, struct xdg_toplevel *t,
                                            int32_t w, int32_t h)
{
    (void)data; (void)t; (void)w; (void)h;
}
static void on_toplevel_wm_capabilities(void *data, struct xdg_toplevel *t,
                                           struct wl_array *caps)
{
    (void)data; (void)t; (void)caps;
}
static const struct xdg_toplevel_listener tl_lis = {
    .configure        = on_toplevel_configure,
    .close            = on_toplevel_close,
    .configure_bounds = on_toplevel_configure_bounds,
    .wm_capabilities  = on_toplevel_wm_capabilities,
};

/* ------------------------------------------------------------------ */
/* Wayland setup                                                       */
/* ------------------------------------------------------------------ */

static int wayland_init(const char *title, int w, int h)
{
    if (!getenv("WAYLAND_DISPLAY")) setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) {
        fprintf(stderr, "[brook-player] wl_display_connect failed: %s\n",
                strerror(errno));
        return -1;
    }

    g_reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(g_reg, &reg_lis, NULL);
    wl_display_roundtrip(g_dpy);

    if (!g_shm || !g_comp || !g_wm) {
        fprintf(stderr, "[brook-player] missing Wayland globals "
                "shm=%p comp=%p wm=%p\n",
                (void*)g_shm, (void*)g_comp, (void*)g_wm);
        return -1;
    }

    xdg_wm_base_add_listener(g_wm, &wm_lis, NULL);

    g_surf  = wl_compositor_create_surface(g_comp);
    g_xsurf = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    g_tl    = xdg_surface_get_toplevel(g_xsurf);

    xdg_surface_add_listener(g_xsurf, &xs_lis, NULL);
    xdg_toplevel_add_listener(g_tl, &tl_lis, NULL);
    xdg_toplevel_set_title(g_tl, title);
    xdg_toplevel_set_app_id(g_tl, "brook-player");

    if (g_fullscreen)
        xdg_toplevel_set_fullscreen(g_tl, NULL);

    wl_surface_commit(g_surf);
    wl_display_roundtrip(g_dpy);

    /* Wait for first configure */
    for (int i = 0; i < 50 && !g_configured; i++) {
        wl_display_dispatch(g_dpy);
        sleep_ms(20);
    }
    if (!g_configured) {
        fprintf(stderr, "[brook-player] timeout waiting for configure\n");
        return -1;
    }

    /* If compositor didn't specify a size, use video dimensions */
    if (g_width <= 0)  g_width  = w;
    if (g_height <= 0) g_height = h;

    /* Create double buffers */
    if (shm_bufs_create(g_width, g_height) < 0) {
        fprintf(stderr, "[brook-player] failed to create shm buffers\n");
        return -1;
    }

    fprintf(stderr, "[brook-player] Wayland init ok: %dx%d\n",
            g_width, g_height);
    return 0;
}

static void wayland_cleanup(void)
{
    for (int i = 0; i < 2; i++)
        shm_buf_destroy(&g_bufs[i]);
    if (g_tl)    xdg_toplevel_destroy(g_tl);
    if (g_xsurf) xdg_surface_destroy(g_xsurf);
    if (g_surf)  wl_surface_destroy(g_surf);
    if (g_dpy)   wl_display_disconnect(g_dpy);
}

/* ------------------------------------------------------------------ */
/* Frame display                                                       */
/* ------------------------------------------------------------------ */

static void display_frame(uint8_t *rgb_data, int linesize)
{
    ShmBuffer *b = &g_bufs[g_back];

    /* Wait for this buffer to be released by the server */
    while (b->busy) {
        wl_display_dispatch(g_dpy);
    }

    /* Copy scaled frame into the shm buffer (BGRA → XRGB8888, same layout) */
    int dst_stride = g_width * 4;
    for (int y = 0; y < g_height; y++) {
        memcpy(b->pixels + y * dst_stride,
               rgb_data + y * linesize,
               dst_stride);
    }

    wl_surface_attach(g_surf, b->wl_buf, 0, 0);
    wl_surface_damage(g_surf, 0, 0, g_width, g_height);
    wl_surface_commit(g_surf);
    b->busy = 1;

    /* Swap back buffer index */
    g_back = 1 - g_back;

    wl_display_flush(g_dpy);
}

/* ------------------------------------------------------------------ */
/* Audio output                                                        */
/* ------------------------------------------------------------------ */

static int audio_init(AVFormatContext *fmtCtx)
{
    /* Find audio stream */
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            g_audIdx = (int)i;
            g_audStream = fmtCtx->streams[i];
            break;
        }
    }
    if (g_audIdx < 0) {
        fprintf(stderr, "[brook-player] no audio stream found\n");
        return -1;
    }

    /* Open audio decoder */
    const AVCodec *acodec = avcodec_find_decoder(g_audStream->codecpar->codec_id);
    if (!acodec) {
        fprintf(stderr, "[brook-player] unsupported audio codec\n");
        return -1;
    }

    g_audDecCtx = avcodec_alloc_context3(acodec);
    avcodec_parameters_to_context(g_audDecCtx, g_audStream->codecpar);
    if (avcodec_open2(g_audDecCtx, acodec, NULL) < 0) {
        fprintf(stderr, "[brook-player] failed to open audio codec\n");
        return -1;
    }

    fprintf(stderr, "[brook-player] audio: %d Hz, %d ch, fmt=%d codec=%s\n",
            g_audDecCtx->sample_rate,
            g_audDecCtx->ch_layout.nb_channels,
            g_audDecCtx->sample_fmt,
            acodec->name);

    /* Set up resampler: source format → S16_LE / 44100 / stereo */
    g_swr = swr_alloc();
    if (!g_swr) {
        fprintf(stderr, "[brook-player] swr_alloc failed\n");
        return -1;
    }

    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_layout;
    if (g_audDecCtx->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&in_layout, &g_audDecCtx->ch_layout);
    } else {
        av_channel_layout_default(&in_layout, 2);
    }

    swr_alloc_set_opts2(&g_swr,
                        &out_layout, AUDIO_FORMAT, AUDIO_RATE,
                        &in_layout, g_audDecCtx->sample_fmt,
                        g_audDecCtx->sample_rate,
                        0, NULL);

    if (swr_init(g_swr) < 0) {
        fprintf(stderr, "[brook-player] swr_init failed\n");
        swr_free(&g_swr);
        g_swr = NULL;
        return -1;
    }

    /* Open /dev/dsp for raw PCM output */
    g_dsp_fd = open("/dev/dsp", O_WRONLY);
    if (g_dsp_fd < 0) {
        fprintf(stderr, "[brook-player] failed to open /dev/dsp: %s\n",
                strerror(errno));
        return -1;
    }

    fprintf(stderr, "[brook-player] audio output: /dev/dsp fd=%d "
            "(S16_LE %d Hz stereo)\n", g_dsp_fd, AUDIO_RATE);
    return 0;
}

static void audio_decode_packet(AVPacket *pkt)
{
    if (!g_audDecCtx || !g_swr || g_dsp_fd < 0)
        return;

    int ret = avcodec_send_packet(g_audDecCtx, pkt);
    if (ret < 0) return;

    AVFrame *aframe = av_frame_alloc();
    while (avcodec_receive_frame(g_audDecCtx, aframe) == 0) {
        /* Calculate output sample count after resampling */
        int out_samples = swr_get_out_samples(g_swr, aframe->nb_samples);
        if (out_samples <= 0) continue;

        /* Allocate output buffer: S16_LE, stereo = 4 bytes per sample */
        int buf_size = out_samples * AUDIO_CHANNELS * 2;
        uint8_t *out_buf = malloc(buf_size);
        if (!out_buf) continue;

        uint8_t *out_planes[1] = { out_buf };
        int converted = swr_convert(g_swr, out_planes, out_samples,
                                    (const uint8_t **)aframe->extended_data,
                                    aframe->nb_samples);
        if (converted > 0) {
            int bytes = converted * AUDIO_CHANNELS * 2;
            (void)!write(g_dsp_fd, out_buf, bytes);
        }

        free(out_buf);
    }
    av_frame_free(&aframe);
}

static void audio_cleanup(void)
{
    if (g_swr) swr_free(&g_swr);
    if (g_audDecCtx) avcodec_free_context(&g_audDecCtx);
    if (g_dsp_fd >= 0) close(g_dsp_fd);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *filename = NULL;
    int autoexit = 1;
    (void)autoexit; /* always auto-exit for now */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-fs"))
            g_fullscreen = 1;
        else if (!strcmp(argv[i], "-autoexit"))
            autoexit = 1;
        else if (argv[i][0] != '-')
            filename = argv[i];
    }

    if (!filename) {
        fprintf(stderr, "Usage: brook-player [-fs] [-autoexit] <video-file>\n");
        return 1;
    }

    /* Give waylandd time to start if launched concurrently */
    sleep_ms(500);

    fprintf(stderr, "[brook-player] opening %s\n", filename);

    /* ----- Open video file ----- */
    AVFormatContext *fmtCtx = NULL;
    if (avformat_open_input(&fmtCtx, filename, NULL, NULL) < 0) {
        fprintf(stderr, "[brook-player] failed to open %s\n", filename);
        return 1;
    }
    if (avformat_find_stream_info(fmtCtx, NULL) < 0) {
        fprintf(stderr, "[brook-player] failed to find stream info\n");
        return 1;
    }

    /* Find video stream */
    int vidIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vidIdx = (int)i;
            break;
        }
    }
    if (vidIdx < 0) {
        fprintf(stderr, "[brook-player] no video stream found\n");
        return 1;
    }

    AVStream *vidStream = fmtCtx->streams[vidIdx];
    const AVCodec *codec = avcodec_find_decoder(vidStream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "[brook-player] unsupported codec\n");
        return 1;
    }

    AVCodecContext *decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decCtx, vidStream->codecpar);
    if (avcodec_open2(decCtx, codec, NULL) < 0) {
        fprintf(stderr, "[brook-player] failed to open codec\n");
        return 1;
    }

    g_vid_w = decCtx->width;
    g_vid_h = decCtx->height;
    fprintf(stderr, "[brook-player] video: %dx%d codec=%s\n",
            g_vid_w, g_vid_h, codec->name);

    /* ----- Audio init ----- */
    audio_init(fmtCtx);

    /* ----- Wayland init ----- */
    {
        /* Derive a title from filename */
        const char *base = strrchr(filename, '/');
        char title[256];
        snprintf(title, sizeof(title), "brook-player — %s",
                 base ? base + 1 : filename);
        if (wayland_init(title, g_vid_w, g_vid_h) < 0)
            return 1;
    }

    /* ----- Set up scaler ----- */
    struct SwsContext *swsCtx = sws_getContext(
        g_vid_w, g_vid_h, decCtx->pix_fmt,
        g_width, g_height, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!swsCtx) {
        fprintf(stderr, "[brook-player] failed to create scaler\n");
        return 1;
    }

    /* Allocate destination frame for scaled BGRA output */
    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};
    av_image_alloc(dst_data, dst_linesize, g_width, g_height,
                   AV_PIX_FMT_BGRA, 1);

    AVPacket *pkt   = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();

    fprintf(stderr, "[brook-player] starting playback %dx%d → %dx%d\n",
            g_vid_w, g_vid_h, g_width, g_height);

    /* ----- Decode + display loop ----- */
    int64_t start_time = now_us();
    int64_t first_pts  = AV_NOPTS_VALUE;
    int frames_shown = 0;

    while (!g_close_req) {
        /* Drain Wayland events (non-blocking) */
        while (wl_display_prepare_read(g_dpy) != 0)
            wl_display_dispatch_pending(g_dpy);
        wl_display_flush(g_dpy);

        struct pollfd pfd = {
            .fd = wl_display_get_fd(g_dpy),
            .events = POLLIN,
        };
        int pr = poll(&pfd, 1, 0); /* non-blocking check */
        if (pr > 0)
            wl_display_read_events(g_dpy);
        else
            wl_display_cancel_read(g_dpy);
        wl_display_dispatch_pending(g_dpy);

        if (g_close_req) break;

        /* Read next packet */
        int ret = av_read_frame(fmtCtx, pkt);
        if (ret < 0) {
            /* EOF or error */
            fprintf(stderr, "[brook-player] end of file (frames=%d)\n",
                    frames_shown);
            break;
        }

        if (pkt->stream_index == g_audIdx) {
            audio_decode_packet(pkt);
            av_packet_unref(pkt);
            continue;
        }

        if (pkt->stream_index != vidIdx) {
            av_packet_unref(pkt);
            continue;
        }

        /* Decode */
        ret = avcodec_send_packet(decCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (avcodec_receive_frame(decCtx, frame) == 0) {
            /* Scale to display size as BGRA */
            sws_scale(swsCtx,
                      (const uint8_t *const *)frame->data, frame->linesize,
                      0, g_vid_h,
                      dst_data, dst_linesize);

            /* Frame timing: pace output to match PTS */
            int64_t pts = frame->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE) {
                if (first_pts == AV_NOPTS_VALUE) {
                    first_pts  = pts;
                    start_time = now_us();
                }

                /* Convert PTS to microseconds */
                double tb = av_q2d(vidStream->time_base);
                int64_t frame_time_us = (int64_t)((pts - first_pts) * tb * 1e6);
                int64_t elapsed = now_us() - start_time;
                int64_t delay = frame_time_us - elapsed;
                if (delay > 1000 && delay < 2000000) {
                    struct timespec ts = {
                        .tv_sec  = delay / 1000000,
                        .tv_nsec = (delay % 1000000) * 1000
                    };
                    nanosleep(&ts, NULL);
                }
            }

            display_frame(dst_data[0], dst_linesize[0]);
            frames_shown++;

            if (frames_shown % 100 == 0) {
                int64_t elapsed = now_us() - start_time;
                fprintf(stderr, "[brook-player] frame %d, %.1f fps\n",
                        frames_shown,
                        frames_shown * 1e6 / (double)elapsed);
            }
        }
    }

    fprintf(stderr, "[brook-player] done: %d frames displayed\n",
            frames_shown);

    /* Cleanup */
    audio_cleanup();
    av_freep(&dst_data[0]);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(swsCtx);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);
    wayland_cleanup();

    return 0;
}
