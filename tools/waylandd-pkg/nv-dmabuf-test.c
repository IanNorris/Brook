/* nv-drm (renderD130) dmabuf path: modifier helpers, import checks, handle sharing, fence
 * wait and CPU snapshot, with a mock render node. */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>   /* before the mmap rename: the real one stays declared */
static int test_ioctl(int fd, unsigned long request, ...);
static void *test_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
static int test_munmap(void *addr, size_t length);
#define ioctl test_ioctl
#define mmap test_mmap
#define munmap test_munmap
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef munmap
#undef mmap
#undef ioctl

/* mock BOs: handle i (1..7) backs prime fds 10+i; per-BO domain/kind/size; CPU_PREP busy count */
struct mock_bo { int live; uint32_t domain, tile_mode, tile_flags; uint64_t size; int busy, fail; uint8_t *pixels; };
static struct mock_bo bos[8];
static unsigned closes, maps, unmaps, preps;
static int test_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 43);
    va_list args; va_start(args, request); void *arg = va_arg(args, void *); va_end(args);
    if (request == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
        struct drm_prime_handle *ph = arg;
        const int h = ph->fd - 10;
        if (h < 1 || h > 7) { errno = EINVAL; return -1; }
        bos[h].live = 1; ph->handle = (uint32_t)h; return 0;
    }
    if (request == DRM_IOCTL_NOUVEAU_GEM_INFO) {
        struct drm_nouveau_gem_info *gi = arg;
        assert(gi->handle < 8 && bos[gi->handle].live);
        gi->domain = bos[gi->handle].domain; gi->size = bos[gi->handle].size; gi->map_handle = (1ull << 40) + ((uint64_t)gi->handle << 32);
        gi->tile_mode = bos[gi->handle].tile_mode; gi->tile_flags = bos[gi->handle].tile_flags; return 0;
    }
    if (request == DRM_IOCTL_NOUVEAU_GEM_CPU_PREP) {
        struct drm_nouveau_gem_cpu_prep *cp = arg;
        assert(cp->handle < 8 && bos[cp->handle].live && cp->flags == NOUVEAU_GEM_CPU_PREP_NOWAIT);
        ++preps;
        if (bos[cp->handle].fail) { errno = EIO; return -1; }
        if (bos[cp->handle].busy) { --bos[cp->handle].busy; errno = EBUSY; return -1; }
        return 0;
    }
    if (request == DRM_IOCTL_GEM_CLOSE) {
        struct drm_gem_close *gc = arg;
        assert(gc->handle < 8 && bos[gc->handle].live);
        bos[gc->handle].live = 0; ++closes; return 0;
    }
    assert(!"unexpected ioctl"); return -1;
}
static void *test_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)addr;
    if (fd != 43) return mmap(addr, length, prot, flags, fd, offset);
    assert(prot == PROT_READ && (flags & MAP_SHARED));
    const uint32_t h = (uint32_t)(((uint64_t)offset - (1ull << 40)) >> 32);
    assert(h < 8 && bos[h].live && length == bos[h].size);
    ++maps; return bos[h].pixels;
}
static int test_munmap(void *addr, size_t length) { (void)addr; (void)length; ++unmaps; return 0; }

static struct brook_dmabuf_buffer *buf(int w, int h, uint32_t stride, uint64_t modifier)
{
    struct brook_dmabuf_buffer *db = calloc(1, sizeof(*db)); assert(db);
    db->refs = 1; db->width = w; db->height = h; db->stride = stride; db->fourcc = BROOK_DMABUF_XRGB8888; db->modifier = modifier;
    return db;
}
static void mock(int h, uint32_t domain, uint64_t size)
{
    bos[h] = (struct mock_bo){0, domain, 0, 0, size, 0, 0, calloc(1, (size_t)size)};
    for (uint64_t i = 0; i < size / 4; ++i) ((uint32_t *)bos[h].pixels)[i] = 0xff000000u | (uint32_t)i;
}

static void test_modifiers(void)
{
    /* DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, h): the values Linux/NVK use on Turing */
    assert(brook_nv_turing_modifier(0) == 0x0300000000606015ull);
    assert(brook_nv_turing_modifier(5) == 0x0300000000606010ull);
    for (unsigned i = 0; i < BROOK_NV_MOD_COUNT; ++i) {
        struct brook_nv_block_linear b;
        assert(brook_nv_parse_modifier(brook_nv_turing_modifier(i), &b) && b.height_log2 == 5 - i && b.kind == 6 &&
               b.gob_kind == 2 && b.sector == 1 && !b.compression);
        assert(brook_nv_turing_modifier_supported(brook_nv_turing_modifier(i)));
    }
    /* Blackwell 8 bpp sector layout (2) round-trips through the split field */
    struct brook_nv_block_linear b;
    assert(brook_nv_parse_modifier(brook_nv_block_linear_2d(0, 2, 2, 6, 4), &b) && b.sector == 2);
    assert(!brook_nv_turing_modifier_supported(brook_nv_block_linear_2d(0, 2, 2, 6, 4)));
    assert(!brook_nv_turing_modifier_supported(brook_nv_block_linear_2d(0, 1, 1, 0xfe, 4)));   /* Fermi/G80 kind */
    assert(!brook_nv_turing_modifier_supported(brook_nv_block_linear_2d(1, 1, 2, 6, 4)));      /* compressed */
    assert(!brook_nv_parse_modifier(0, NULL) && !brook_nv_parse_modifier(BROOK_DMABUF_MOD_IMPLICIT, NULL));
    assert(!brook_nv_parse_modifier(0x0300000000606015ull | (1ull << 8), NULL));               /* reserved bit */
    assert(!brook_nv_parse_modifier(0x0100000000000001ull, NULL));                             /* Intel X-tiled */
    /* 1600x1200 XRGB, 16-GOB blocks: 100 x 10 blocks of 8 KiB */
    assert(brook_nv_block_linear_bytes(6400, 1200, 4) == 100ull * 10 * 8192);
    assert(brook_nv_block_linear_bytes(64, 1, 0) == 512);
}

int main(void)
{
    test_modifiers();
    g_nv_fd = 43;
    /* a linear GART buffer: accepted; a second wl_buffer on the same BO shares the handle */
    mock(1, NOUVEAU_GEM_DOMAIN_GART, 64 * 64 * 4);
    struct brook_dmabuf_buffer *a = buf(64, 64, 256, BROOK_DMABUF_MOD_IMPLICIT), *a2 = buf(64, 32, 256, BROOK_DMABUF_MOD_LINEAR);
    assert(nv_dmabuf_import(11, a) && a->nv_bo == 1 && nv_dmabuf_import(11, a2) && a2->nv_bo == 1);
    assert(g_nv_imports && g_nv_imports->refs == 2 && closes == 0);
    /* snapshot after the producer fence: two busy polls, then the pixels */
    bos[1].busy = 2; preps = 0;
    int uncertain = 1;
    struct brook_image *im = nv_dmabuf_snapshot(a, &uncertain);
    assert(im && !uncertain && preps == 3 && maps == 1 && im->width == 64 && im->height == 64);
    image_unref(im);
    im = nv_dmabuf_snapshot(a2, &uncertain); assert(im && maps == 1);   /* the mapping is cached per BO */
    image_unref(im);
    /* a failed producer fence withholds the release */
    bos[1].fail = 1; assert(!nv_dmabuf_snapshot(a, &uncertain) && uncertain); bos[1].fail = 0;
    /* releases: the handle closes with the last wl_buffer */
    dmabuf_buffer_unref(a2); assert(closes == 0 && g_nv_imports->refs == 1);
    dmabuf_buffer_unref(a); assert(closes == 1 && unmaps == 1 && !g_nv_imports);
    /* refusals: VRAM, tiled kind, tiled tile_mode, too small, block-linear modifier, bad fd */
    mock(2, 2 /* VRAM */, 65536); struct brook_dmabuf_buffer *v = buf(64, 64, 256, 0);
    assert(!nv_dmabuf_import(12, v) && !v->nv_bo && closes == 2 && !g_nv_imports);
    mock(3, NOUVEAU_GEM_DOMAIN_GART, 65536); bos[3].tile_flags = 0x06u << 8; struct brook_dmabuf_buffer *k = buf(64, 64, 256, 0);
    assert(!nv_dmabuf_import(13, k) && closes == 3);
    mock(4, NOUVEAU_GEM_DOMAIN_GART, 65536); bos[4].tile_mode = 4; assert(!nv_dmabuf_import(14, k) && closes == 4);
    mock(5, NOUVEAU_GEM_DOMAIN_GART, 4096); struct brook_dmabuf_buffer *s = buf(64, 64, 256, 0);
    assert(!nv_dmabuf_import(15, s) && closes == 5);
    mock(6, NOUVEAU_GEM_DOMAIN_GART, 65536); struct brook_dmabuf_buffer *bl = buf(64, 64, 256, brook_nv_turing_modifier(1));
    assert(!nv_dmabuf_import(16, bl) && closes == 6);
    assert(!nv_dmabuf_import(30, s));
    /* a refused second import of a BO in use keeps the first user's handle */
    mock(7, NOUVEAU_GEM_DOMAIN_GART, 16384); struct brook_dmabuf_buffer *g = buf(64, 16, 256, 0), *big = buf(64, 80, 256, 0);
    assert(nv_dmabuf_import(17, g) && !nv_dmabuf_import(17, big) && bos[7].live && g_nv_imports->refs == 1);
    dmabuf_buffer_unref(g); assert(!bos[7].live && !g_nv_imports);
    free(v); free(k); free(s); free(bl); free(big);
    /* the format table: block-linear entries only when asked for */
    assert(!nv_dmabuf_block_linear_advertised());
    setenv("BROOK_WAYLAND_NV_MODIFIERS", "1", 1); assert(nv_dmabuf_block_linear_advertised());
    g_nv_fd = -1; assert(!nv_dmabuf_block_linear_advertised());
    puts("NV_DMABUF_PASS modifiers/import/sharing/fence-wait/snapshot/refusals");
    return 0;
}
