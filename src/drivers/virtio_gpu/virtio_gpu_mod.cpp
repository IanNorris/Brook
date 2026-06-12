// virtio_gpu_mod.cpp — virtio-gpu 2D display driver for QEMU virtio-gpu-pci.
//
// Phase 3 (skeleton): modern virtio-1.0 PCI transport bring-up + controlq
// command submission, proven with GET_DISPLAY_INFO. Resource/scanout/flush
// (Phases 4-5) build on the SubmitCommand path established here.
//
// Modern virtio PCI plumbing mirrors virtio_input_mod.cpp (the correct
// template — virtio-gpu is virtio-1.0 only, no legacy I/O-port BAR).
// DisplayOps registration (Phase 5) will mirror bochs_display_mod.cpp.
//
// virtio-gpu spec: virtio 1.1 §5.7.  QEMU device: virtio-gpu-pci (1af4:1050).
// See VIRTIO_GPU_DOCS.md for the full design + command protocol.

#include "module_abi.h"
#include "pci.h"
#include "serial.h"
#include "kprintf.h"
#include "display.h"
#include "gpu_compositor.h"
#include "gpu_app.h"
#include "tty.h"
#include "compositor.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include "memory/address.h"
#include "mem_tag.h"
#include "string.h"
#include "spinlock.h"

MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PciEnableMemSpace);
MODULE_IMPORT_SYMBOL(PciEnableBusMaster);
MODULE_IMPORT_SYMBOL(PciConfigRead32);
MODULE_IMPORT_SYMBOL(PciConfigRead16);
MODULE_IMPORT_SYMBOL(PciConfigRead8);
MODULE_IMPORT_SYMBOL(PciConfigWrite16);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(SerialPuts);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(VmmAllocPages);
MODULE_IMPORT_SYMBOL(VmmFreePages);
MODULE_IMPORT_SYMBOL(VmmVirtToPhys);
MODULE_IMPORT_SYMBOL(VmmMapPage);
MODULE_IMPORT_SYMBOL(PmmAllocPages);
MODULE_IMPORT_SYMBOL(DisplayRegister);
MODULE_IMPORT_SYMBOL(DisplaySet3DActive);
MODULE_IMPORT_SYMBOL(GpuCompositorRegister);
MODULE_IMPORT_SYMBOL(GpuAppRegister);
MODULE_IMPORT_SYMBOL(TtyGetFramebuffer);
MODULE_IMPORT_SYMBOL(TtyRemap);
MODULE_IMPORT_SYMBOL(CompositorRemap);
MODULE_IMPORT_SYMBOL(PmmAllocPages);

using namespace brook;

// ---------------------------------------------------------------------------
// Modern virtio PCI capability types (virtio 1.0 §4.1.4)
// ---------------------------------------------------------------------------

static constexpr uint8_t VIRTIO_PCI_CAP_COMMON_CFG = 1;
static constexpr uint8_t VIRTIO_PCI_CAP_NOTIFY_CFG = 2;
static constexpr uint8_t VIRTIO_PCI_CAP_ISR_CFG    = 3;
static constexpr uint8_t VIRTIO_PCI_CAP_DEVICE_CFG = 4;

// Common config layout offsets (virtio 1.0 §4.1.4.3)
enum VirtioCommonReg : uint32_t {
    VIRTIO_COMMON_DFSELECT     = 0x00,
    VIRTIO_COMMON_DF           = 0x04,
    VIRTIO_COMMON_GFSELECT     = 0x08,
    VIRTIO_COMMON_GF           = 0x0C,
    VIRTIO_COMMON_NUM_QUEUES   = 0x12,
    VIRTIO_COMMON_STATUS       = 0x14,
    VIRTIO_COMMON_Q_SELECT     = 0x16,
    VIRTIO_COMMON_Q_SIZE       = 0x18,
    VIRTIO_COMMON_Q_ENABLE     = 0x1C,
    VIRTIO_COMMON_Q_NOTIFY_OFF = 0x1E,
    VIRTIO_COMMON_Q_DESC       = 0x20,
    VIRTIO_COMMON_Q_AVAIL      = 0x28,
    VIRTIO_COMMON_Q_USED       = 0x30,
};

static constexpr uint8_t VIRTIO_STATUS_ACKNOWLEDGE = 1;
static constexpr uint8_t VIRTIO_STATUS_DRIVER      = 2;
static constexpr uint8_t VIRTIO_STATUS_DRIVER_OK   = 4;
static constexpr uint8_t VIRTIO_STATUS_FEATURES_OK = 8;

static constexpr uint16_t VIRTQ_DESC_F_NEXT  = 1;
static constexpr uint16_t VIRTQ_DESC_F_WRITE = 2;

// Avail-ring flag: tell the device not to interrupt on used-ring updates for
// this queue (we poll instead). virtio 1.0 §2.6.7.
static constexpr uint16_t VIRTQ_AVAIL_F_NO_INTERRUPT = 1;

// ---------------------------------------------------------------------------
// virtio-gpu control protocol (virtio 1.1 §5.7.6) — see VIRTIO_GPU_DOCS.md
// ---------------------------------------------------------------------------

static constexpr uint32_t VIRTIO_GPU_CMD_GET_DISPLAY_INFO      = 0x0100;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    = 0x0101;
static constexpr uint32_t VIRTIO_GPU_CMD_SET_SCANOUT           = 0x0103;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_FLUSH        = 0x0104;
static constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   = 0x0105;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;
static constexpr uint32_t VIRTIO_GPU_CMD_GET_CAPSET_INFO      = 0x0108;
static constexpr uint32_t VIRTIO_GPU_CMD_GET_CAPSET           = 0x0109;
static constexpr uint32_t VIRTIO_GPU_RESP_OK_NODATA           = 0x1100;
static constexpr uint32_t VIRTIO_GPU_RESP_OK_DISPLAY_INFO     = 0x1101;
static constexpr uint32_t VIRTIO_GPU_RESP_OK_CAPSET_INFO      = 0x1102;
static constexpr uint32_t VIRTIO_GPU_RESP_OK_CAPSET           = 0x1103;
static constexpr uint32_t VIRTIO_GPU_MAX_SCANOUTS            = 16;

// virtio-gpu 3D feature bits (page 0 / low 32, virtio 1.2 §5.7.3). These gate
// the Virgl/Venus 3D transport the GPU-accel roadmap is built on.
static constexpr uint32_t VIRTIO_GPU_F_VIRGL         = 1u << 0;  // 3D rendering (Virgl/Venus capsets)
static constexpr uint32_t VIRTIO_GPU_F_EDID          = 1u << 1;  // GET_EDID
static constexpr uint32_t VIRTIO_GPU_F_RESOURCE_UUID = 1u << 2;  // ASSIGN_UUID (dmabuf export)
static constexpr uint32_t VIRTIO_GPU_F_RESOURCE_BLOB = 1u << 3;  // blob resources (host-visible memory)
static constexpr uint32_t VIRTIO_GPU_F_CONTEXT_INIT  = 1u << 4;  // per-context capset selection (Venus needs this)

// Capset ids reported by GET_CAPSET_INFO (Linux drm/virtgpu + Mesa).
static constexpr uint32_t VIRTIO_GPU_CAPSET_VIRGL       = 1;
static constexpr uint32_t VIRTIO_GPU_CAPSET_VIRGL2      = 2;
static constexpr uint32_t VIRTIO_GPU_CAPSET_GFXSTREAM   = 3;
static constexpr uint32_t VIRTIO_GPU_CAPSET_VENUS       = 4;
static constexpr uint32_t VIRTIO_GPU_CAPSET_CROSS_DOMAIN= 5;
static constexpr uint32_t VIRTIO_GPU_CAPSET_DRM         = 6;

// Brook framebuffer is Bgr8 (memory bytes B,G,R,X) → B8G8R8X8_UNORM.
static constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM   = 2;

// 3D (virgl) control commands — virtio 1.2 §5.7.6 / Linux virtio_gpu uapi. Only
// meaningful once VIRGL is negotiated; gated behind g_gpu3dFeatures.
static constexpr uint32_t VIRTIO_GPU_CMD_CTX_CREATE            = 0x0200;
static constexpr uint32_t VIRTIO_GPU_CMD_CTX_DESTROY          = 0x0201;
static constexpr uint32_t VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE   = 0x0202;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_CREATE_3D    = 0x0204;
static constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D   = 0x0205;
static constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D = 0x0206;
static constexpr uint32_t VIRTIO_GPU_CMD_SUBMIT_3D             = 0x0207;

// Single framebuffer resource id used for scanout 0.
static constexpr uint32_t RESOURCE_FB = 1;

struct __attribute__((packed)) VirtioGpuCtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
};

struct __attribute__((packed)) VirtioGpuRect {
    uint32_t x, y, width, height;
};

struct __attribute__((packed)) VirtioGpuRespDisplayInfo {
    VirtioGpuCtrlHdr hdr;
    struct __attribute__((packed)) {
        VirtioGpuRect r;
        uint32_t      enabled;
        uint32_t      flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct __attribute__((packed)) VirtioGpuResourceCreate2D {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct __attribute__((packed)) VirtioGpuMemEntry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

// Followed immediately by nr_entries × VirtioGpuMemEntry in the same buffer.
struct __attribute__((packed)) VirtioGpuResourceAttachBacking {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
};

struct __attribute__((packed)) VirtioGpuSetScanout {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuRect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct __attribute__((packed)) VirtioGpuTransferToHost2D {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuRect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct __attribute__((packed)) VirtioGpuResourceFlush {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuRect r;
    uint32_t resource_id;
    uint32_t padding;
};

// GET_CAPSET_INFO: enumerate the host 3D capsets (virgl, virgl2, venus, …).
struct __attribute__((packed)) VirtioGpuGetCapsetInfo {
    VirtioGpuCtrlHdr hdr;
    uint32_t capset_index;
    uint32_t padding;
};

struct __attribute__((packed)) VirtioGpuRespCapsetInfo {
    VirtioGpuCtrlHdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
};

// GET_CAPSET: fetch a capset blob (virgl_caps) by id+version. The response is a
// ctrl header followed by capset_data[] (the blob).
struct __attribute__((packed)) VirtioGpuGetCapset {
    VirtioGpuCtrlHdr hdr;
    uint32_t capset_id;
    uint32_t capset_version;
};

// 3D (virgl) command structs — Linux virtio_gpu uapi layout.
struct __attribute__((packed)) VirtioGpuCtxCreate {
    VirtioGpuCtrlHdr hdr;
    uint32_t nlen;
    uint32_t context_init;   // low 8 bits = capset_id when F_CONTEXT_INIT
    char     debug_name[64];
};

struct __attribute__((packed)) VirtioGpuCtxResource {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct __attribute__((packed)) VirtioGpuResourceCreate3D {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t target;         // PIPE_TEXTURE_2D
    uint32_t format;         // VIRGL_FORMAT_*
    uint32_t bind;           // VIRGL_BIND_*
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
};

struct __attribute__((packed)) VirtioGpuBox {
    uint32_t x, y, z;
    uint32_t w, h, d;
};

struct __attribute__((packed)) VirtioGpuTransferHost3D {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuBox box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
};

// SUBMIT_3D: followed immediately by `size` bytes of virgl command-stream dwords
// in the same request buffer.
struct __attribute__((packed)) VirtioGpuCmdSubmit {
    VirtioGpuCtrlHdr hdr;
    uint32_t size;
    uint32_t padding;
};

// --- Gallium/virgl encoding constants (virgl_hw.h / virgl_protocol.h) ---
static constexpr uint32_t VIRGL_FORMAT_B8G8R8X8_UNORM = 2;
static constexpr uint32_t VIRGL_FORMAT_B8G8R8A8_UNORM = 1;
static constexpr uint32_t PIPE_TEXTURE_2D             = 2;
static constexpr uint32_t VIRGL_BIND_RENDER_TARGET    = 1u << 1;
static constexpr uint32_t VIRGL_BIND_SAMPLER_VIEW     = 1u << 3;
static constexpr uint32_t VIRGL_BIND_SCANOUT          = 1u << 18;
static constexpr uint32_t PIPE_CLEAR_COLOR0           = 1u << 2;

// virgl command-stream opcodes + object types.
static constexpr uint32_t VIRGL_CCMD_CREATE_OBJECT        = 1;
static constexpr uint32_t VIRGL_CCMD_BIND_OBJECT          = 2;
static constexpr uint32_t VIRGL_CCMD_SET_VIEWPORT_STATE   = 4;
static constexpr uint32_t VIRGL_CCMD_SET_FRAMEBUFFER_STATE = 5;
static constexpr uint32_t VIRGL_CCMD_SET_VERTEX_BUFFERS   = 6;
static constexpr uint32_t VIRGL_CCMD_CLEAR               = 7;
static constexpr uint32_t VIRGL_CCMD_DRAW_VBO            = 8;
static constexpr uint32_t VIRGL_CCMD_SET_SAMPLER_VIEWS   = 10;
static constexpr uint32_t VIRGL_CCMD_BLIT                = 16;
static constexpr uint32_t VIRGL_CCMD_BIND_SAMPLER_STATES = 18;
static constexpr uint32_t VIRGL_CCMD_BIND_SHADER         = 31;

static constexpr uint32_t VIRGL_OBJECT_BLEND            = 1;
static constexpr uint32_t VIRGL_OBJECT_RASTERIZER       = 2;
static constexpr uint32_t VIRGL_OBJECT_DSA              = 3;
static constexpr uint32_t VIRGL_OBJECT_SHADER          = 4;
static constexpr uint32_t VIRGL_OBJECT_VERTEX_ELEMENTS = 5;
static constexpr uint32_t VIRGL_OBJECT_SAMPLER_VIEW    = 6;
static constexpr uint32_t VIRGL_OBJECT_SAMPLER_STATE   = 7;
static constexpr uint32_t VIRGL_OBJECT_SURFACE           = 8;

// Extra formats / binds / pipe enums for the DRAW (textured-quad) path.
static constexpr uint32_t VIRGL_FORMAT_R32G32_FLOAT   = 29;   // vertex pos/uv attribute
static constexpr uint32_t VIRGL_FORMAT_R32_FLOAT      = 28;   // vertex opacity attribute
static constexpr uint32_t VIRGL_FORMAT_R8_UNORM       = 64;   // raw byte buffer element
static constexpr uint32_t VIRGL_BIND_VERTEX_BUFFER    = 1u << 4;
static constexpr uint32_t PIPE_BUFFER                 = 0;    // resource target
static constexpr uint32_t PIPE_SHADER_VERTEX          = 0;
static constexpr uint32_t PIPE_SHADER_FRAGMENT        = 1;
static constexpr uint32_t PIPE_PRIM_TRIANGLE_STRIP    = 5;
// Gallium blend factors / function for standard src-alpha-over composite.
static constexpr uint32_t PIPE_BLENDFACTOR_SRC_ALPHA     = 0x3;
static constexpr uint32_t PIPE_BLENDFACTOR_INV_SRC_ALPHA = 0x13;
static constexpr uint32_t PIPE_BLEND_ADD                 = 0;

// virgl BLIT: copy mask for an RGBA colour blit (PIPE_MASK_RGBA) and a
// nearest-filter (PIPE_TEX_FILTER_NEAREST) — exact 1:1 pixel copy.
static constexpr uint32_t VIRGL_BLIT_MASK_RGBA = 0xF;
static constexpr uint32_t VIRGL_TEX_FILTER_NEAREST = 0;
static constexpr uint32_t VIRGL_TEX_FILTER_LINEAR  = 1;

// virgl command header: cmd | (obj_type << 8) | (len_in_dwords << 16).
static inline uint32_t VirglCmd0(uint32_t cmd, uint32_t obj, uint32_t len)
{ return cmd | (obj << 8) | (len << 16); }

static inline uint32_t F32Bits(float f)
{ uint32_t u; __builtin_memcpy(&u, &f, sizeof(u)); return u; }

// Convert a signed integer to its IEEE-754 single-precision bit pattern using
// only integer ops (the kernel is built -mno-sse / soft-float-free, so runtime
// floating-point math is unavailable). Exact for |v| <= 2^24, which covers all
// screen-pixel coordinates and NDC numerators at 1080p and beyond. Lets the
// compositor build per-quad vertex buffers (clip-space positions derived from
// integer pixel rects) without any runtime FP.
static inline uint32_t IntToF32Bits(int32_t v)
{
    if (v == 0) return 0;
    uint32_t sign = 0, a;
    if (v < 0) { sign = 0x80000000u; a = static_cast<uint32_t>(-static_cast<int64_t>(v)); }
    else       { a = static_cast<uint32_t>(v); }
    int e = 0; uint32_t m = a;
    while (m >= (1u << 24)) { m >>= 1; ++e; }   // shrink: too many bits
    while (m <  (1u << 23)) { m <<= 1; --e; }   // grow: normalise top bit -> bit 23
    uint32_t exp = static_cast<uint32_t>(23 + e + 127);
    return sign | (exp << 23) | (m & 0x7FFFFFu);
}

// IEEE-754 bit pattern of the fraction num/den, computed in integer/fixed-point
// (no runtime FP). Used for texture uv coordinates (s/texDim) and, via NdcBits,
// for clip-space vertex positions. Exact when num/den is a dyadic-friendly ratio
// at these magnitudes; |err| < 1.2e-7 otherwise.
static inline uint32_t FracBits(int32_t num, int32_t den)
{
    if (num == 0 || den == 0) return 0;
    int64_t q = (static_cast<int64_t>(num) << 23) / den;   // ~ (num/den) * 2^23
    if (q == 0) return 0;
    uint32_t f = IntToF32Bits(static_cast<int32_t>(q));
    uint32_t exp = (f >> 23) & 0xFF;                 // divide by 2^23 ...
    exp -= 23;                                       // ... via exponent bias
    return (f & 0x807FFFFFu) | (exp << 23);
}

// Clip-space (NDC) bit pattern for pixel coordinate `px` on an axis of size
// `dim`: ndc = 2*px/dim - 1. This is how the compositor turns integer window
// pixel rects into vertex positions for the DRAW path.
static inline uint32_t NdcBits(int32_t px, int32_t dim)
{
    return FracBits(2 * px - dim, dim);
}

// Self-test context/resource ids and render-target dimension.
static constexpr uint32_t CTX_ID_SELFTEST = 1;
static constexpr uint32_t RES_3D_SELFTEST = 2;   // src (cleared green)
static constexpr uint32_t RES_3D_BLITDST  = 3;   // blit target (blue + green square)
static constexpr uint32_t RES_3D_SCANOUT  = 4;   // scanout-RT present-path gate test
static constexpr uint32_t SURF_SRC        = 1;   // surface handle for src clear
static constexpr uint32_t SURF_DST        = 2;   // surface handle for dst clear
static constexpr uint32_t SURF_SCANOUT    = 3;   // surface handle for scanout clear
static constexpr uint32_t SELFTEST_DIM    = 64;
static constexpr uint32_t SCANOUT_TEST_DIM = 256; // gate-test scanout RT size

// virtio-gpu device config (virtio 1.2 §5.7.4). num_capsets is meaningful only
// when VIRGL is negotiated (0 on a plain 2D device).
enum VirtioGpuConfigReg : uint32_t {
    VIRTIO_GPU_CFG_EVENTS_READ  = 0x00,
    VIRTIO_GPU_CFG_EVENTS_CLEAR = 0x04,
    VIRTIO_GPU_CFG_NUM_SCANOUTS = 0x08,
    VIRTIO_GPU_CFG_NUM_CAPSETS  = 0x0C,
};

// ---------------------------------------------------------------------------
// Virtqueue structures
// ---------------------------------------------------------------------------

struct __attribute__((packed)) VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct __attribute__((packed)) VirtqUsedElem {
    uint32_t id;
    uint32_t len;
};

static constexpr uint32_t MAX_QUEUE_SIZE = 64;

// ---------------------------------------------------------------------------
// MMIO accessors
// ---------------------------------------------------------------------------

static inline void mmio_write8(volatile uint8_t* b, uint32_t o, uint8_t v)
{ *reinterpret_cast<volatile uint8_t*>(b + o) = v; }
static inline void mmio_write16(volatile uint8_t* b, uint32_t o, uint16_t v)
{ *reinterpret_cast<volatile uint16_t*>(b + o) = v; }
static inline void mmio_write32(volatile uint8_t* b, uint32_t o, uint32_t v)
{ *reinterpret_cast<volatile uint32_t*>(b + o) = v; }
static inline void mmio_write64(volatile uint8_t* b, uint32_t o, uint64_t v)
{ *reinterpret_cast<volatile uint64_t*>(b + o) = v; }
static inline uint8_t mmio_read8(volatile uint8_t* b, uint32_t o)
{ return *reinterpret_cast<volatile uint8_t*>(b + o); }
static inline uint16_t mmio_read16(volatile uint8_t* b, uint32_t o)
{ return *reinterpret_cast<volatile uint16_t*>(b + o); }
static inline uint32_t mmio_read32(volatile uint8_t* b, uint32_t o)
{ return *reinterpret_cast<volatile uint32_t*>(b + o); }

// ---------------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------------

static volatile uint8_t* g_commonCfg = nullptr;
static volatile uint8_t* g_notifyCfg = nullptr;
static volatile uint8_t* g_isrCfg    = nullptr;
static volatile uint8_t* g_deviceCfg = nullptr;
static uint32_t          g_notifyMultiplier = 0;
static uint16_t          g_queueNotifyOff = 0;

// Negotiated 3D feature set (page-0 device features ∩ what we accept). Zero on a
// plain virtio-gpu-pci (2D) device. g_numCapsets is read from device config once
// VIRGL is live and gates the Venus/Virgl bring-up (Phase B onward).
static uint32_t          g_gpu3dFeatures = 0;
static uint32_t          g_numCapsets = 0;
static bool              g_haveVenusCapset = false;
// Set true once the GPU-clear self-test confirms a live host 3D path.
static bool              g_gpu3dWorks = false;
// Set true once Gate #1 (scanout-as-3D-RT present path) is verified.
static bool              g_gpuScanoutRtOk = false;

// controlq (queue 0)
static uint16_t            g_queueSize = 0;
static VirtqDesc*          g_descTable = nullptr;
static uint16_t*           g_availFlags = nullptr;
static uint16_t*           g_availIdx = nullptr;
static uint16_t*           g_availRing = nullptr;
static volatile uint16_t*  g_usedIdx = nullptr;
static VirtqUsedElem*      g_usedRing = nullptr;
static uint16_t            g_availIdxShadow = 0;
static uint16_t            g_usedIdxShadow = 0;

static uint64_t g_descPhys = 0, g_availPhys = 0, g_usedPhys = 0;

// Command request/response buffer — physically contiguous (PmmAllocPages) so a
// large RESOURCE_ATTACH_BACKING request (header + many mem-entries) is valid as
// a single descriptor. Request region at offset 0; response in the last page.
static uint8_t* g_cmdBuf = nullptr;
static uint64_t g_cmdBufPhys = 0;
static constexpr uint32_t CMD_PAGES    = 16;
static constexpr uint32_t CMD_REQ_OFF  = 0;
static constexpr uint32_t CMD_RESP_OFF = (CMD_PAGES - 1) * 4096;
static constexpr uint32_t CMD_RESP_CAP = 4096;

// BRO-189: the control queue + the single shared staging buffer (g_cmdBuf) are a
// global resource, but two independent submitters race on them: the compositor
// kernel thread (virgl ctx 2, presenting every frame) and app processes (e.g.
// glgears, ctx 256) submitting from their own syscall context on other CPUs.
// With no serialization they corrupt each other's command streams and the
// avail/used ring shadows, which the host reports as "Illegal command buffer"
// and which wedges the whole virgl device (both contexts then fail).
//
// Serialize the entire fill-g_cmdBuf + SubmitCommand + read-response critical
// section at the public-op boundary (every GpuAppOps / GpuCompositorOps entry
// that submits). A plain ticket SpinLock is used rather than an IrqSpinLock: no
// GPU submit ever runs in IRQ context (the IRQ path only consumes the used
// ring), so disabling IRQs across a full device round-trip would needlessly
// hurt interrupt latency. The lock is non-recursive; no guarded op calls another
// guarded op (verified), so there is no self-deadlock. Each submit is one
// complete command to an independent host context, so serializing whole ops is
// correct even when the compositor and an app interleave between frames.
static SpinLock g_gpuCmdLock;
struct GpuCmdGuard {
    GpuCmdGuard()  { SpinLockAcquire(&g_gpuCmdLock); }
    ~GpuCmdGuard() { SpinLockRelease(&g_gpuCmdLock); }
};

// Framebuffer resource backing — the front buffer the device scans out of AND
// the surface the compositor renders into. Physically contiguous (PmmAllocPages)
// so it doubles as the compositor's PhysToVirt-mapped framebuffer.
static uint8_t* g_fbBacking = nullptr;
static uint64_t g_fbBackingPhys = 0;
static uint32_t g_fbW = 0, g_fbH = 0;

// Display geometry from GET_DISPLAY_INFO (scanout 0).
static uint32_t g_dispWidth = 0, g_dispHeight = 0;

static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// ---------------------------------------------------------------------------
// PCI capability parsing (virtio 1.0 §4.1.4) — mirror of virtio_input
// ---------------------------------------------------------------------------

struct VirtioPciCap {
    uint32_t offset;
    uint32_t length;
    uint8_t  bar;
};

static bool FindVirtioCaps(const PciDevice& dev, VirtioPciCap caps[5])
{
    uint8_t capPtr = static_cast<uint8_t>(PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x34));
    int found = 0;
    while (capPtr != 0 && capPtr != 0xFF)
    {
        uint8_t capId = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr);
        if (capId == 0x09) // vendor-specific = virtio
        {
            uint8_t cfgType = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 3);
            uint8_t bar     = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 4);
            uint32_t offset = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 8);
            uint32_t length = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 12);
            if (cfgType >= 1 && cfgType <= 4)
            {
                caps[cfgType].bar    = bar;
                caps[cfgType].offset = offset;
                caps[cfgType].length = length;
                ++found;
                SerialPrintf("virtio_gpu: cap type %u bar %u off 0x%x len 0x%x\n",
                             cfgType, bar, offset, length);
                if (cfgType == VIRTIO_PCI_CAP_NOTIFY_CFG)
                    g_notifyMultiplier = PciConfigRead32(dev.bus, dev.dev, dev.fn, capPtr + 16);
            }
        }
        capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 1);
    }
    return found >= 4;
}

static void DisableMsix(const PciDevice& dev)
{
    uint8_t capPtr = static_cast<uint8_t>(PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x34));
    while (capPtr != 0 && capPtr != 0xFF)
    {
        uint8_t capId = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr);
        if (capId == 0x11) // MSI-X
        {
            uint16_t msgCtrl = PciConfigRead16(dev.bus, dev.dev, dev.fn, capPtr + 2);
            if (msgCtrl & 0x8000)
                PciConfigWrite16(dev.bus, dev.dev, dev.fn, capPtr + 2,
                                 msgCtrl & ~static_cast<uint16_t>(0x8000));
            return;
        }
        capPtr = PciConfigRead8(dev.bus, dev.dev, dev.fn, capPtr + 1);
    }
}

static volatile uint8_t* MapBar(const PciDevice& dev, uint8_t barIdx,
                                uint32_t offset, uint32_t length)
{
    uint64_t barPhys = PciBarMemBase32(dev.bar[barIdx]);
    if (PciBarIs64(dev.bar[barIdx]) && barIdx + 1 < 6)
        barPhys |= static_cast<uint64_t>(dev.bar[barIdx + 1]) << 32;

    uint64_t regionPhys = barPhys + offset;
    uint32_t pages = AlignUp(length + (offset & 0xFFF), 4096) / 4096;
    if (pages == 0) pages = 1;

    auto vaddr = VmmAllocPages(pages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!vaddr) return nullptr;

    uint64_t physBase = regionPhys & ~0xFFFULL;
    for (uint32_t i = 0; i < pages; ++i)
        VmmMapPage(KernelPageTable,
                   VirtualAddress(vaddr.raw() + i * 4096),
                   PhysicalAddress(physBase + i * 4096),
                   VMM_WRITABLE | VMM_NO_EXEC | VMM_CACHE_DISABLE,
                   MemTag::Device, KernelPid);

    return reinterpret_cast<volatile uint8_t*>(vaddr.raw() + (regionPhys & 0xFFF));
}

// ---------------------------------------------------------------------------
// controlq allocation
// ---------------------------------------------------------------------------

static bool AllocControlQueue()
{
    uint32_t N = g_queueSize;
    uint32_t descPages  = AlignUp(16 * N, 4096) / 4096;
    uint32_t availPages = AlignUp(6 + 2 * N, 4096) / 4096;
    uint32_t usedPages  = AlignUp(6 + 8 * N, 4096) / 4096;
    uint32_t totalPages = descPages + availPages + usedPages;

    auto qAddr = VmmAllocPages(totalPages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!qAddr) return false;

    uint8_t* base = reinterpret_cast<uint8_t*>(qAddr.raw());
    memset(base, 0, totalPages * 4096);

    uint8_t* descBase  = base;
    uint8_t* availBase = descBase + descPages * 4096;
    uint8_t* usedBase  = availBase + availPages * 4096;

    g_descTable  = reinterpret_cast<VirtqDesc*>(descBase);
    g_availFlags = reinterpret_cast<uint16_t*>(availBase);
    g_availIdx   = reinterpret_cast<uint16_t*>(availBase + 2);
    g_availRing  = reinterpret_cast<uint16_t*>(availBase + 4);
    g_usedIdx    = reinterpret_cast<volatile uint16_t*>(usedBase + 2);
    g_usedRing   = reinterpret_cast<VirtqUsedElem*>(usedBase + 4);

    // We drive the controlq purely by polling the used ring (SubmitCommand),
    // so ask the device never to raise an interrupt for it. Without this the
    // device asserts its (shared, INTx) IRQ line on every command completion;
    // since this driver never reads the ISR to acknowledge it, the line stays
    // asserted → interrupt storm on the shared vector, starving the CPU and
    // hammering the co-resident virtio-input handler (input flood + lockup).
    *g_availFlags = VIRTQ_AVAIL_F_NO_INTERRUPT;

    g_descPhys   = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(descBase))).raw();
    g_availPhys  = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(availBase))).raw();
    g_usedPhys   = VmmVirtToPhys(KernelPageTable, VirtualAddress(reinterpret_cast<uint64_t>(usedBase))).raw();

    // Command buffer: physically contiguous so multi-page requests are valid.
    PhysicalAddress cmdPhys = PmmAllocPages(CMD_PAGES, MemTag::Device, KernelPid);
    if (!cmdPhys) return false;
    g_cmdBufPhys = cmdPhys.raw();
    g_cmdBuf     = reinterpret_cast<uint8_t*>(PhysToVirt(cmdPhys).raw());
    memset(g_cmdBuf, 0, CMD_PAGES * 4096);

    return true;
}

static void NotifyControlQueue()
{
    volatile uint16_t* notifyAddr = reinterpret_cast<volatile uint16_t*>(
        g_notifyCfg + g_queueNotifyOff * g_notifyMultiplier);
    *notifyAddr = 0; // queue index 0
}

// Submit one control command: request of reqLen bytes at g_cmdBuf+CMD_REQ_OFF,
// response written to g_cmdBuf+CMD_RESP_OFF. Returns the response length, or 0
// on timeout. Synchronous poll of the used ring (no IRQs) — the display path
// runs from the compositor thread, never an ISR.
static uint32_t SubmitCommand(uint32_t reqLen, uint32_t respCap)
{
    uint16_t head = static_cast<uint16_t>(g_availIdxShadow % g_queueSize);
    uint16_t d1   = static_cast<uint16_t>((head + 1) % g_queueSize);

    g_descTable[head].addr  = g_cmdBufPhys + CMD_REQ_OFF;
    g_descTable[head].len   = reqLen;
    g_descTable[head].flags = VIRTQ_DESC_F_NEXT;
    g_descTable[head].next  = d1;

    g_descTable[d1].addr  = g_cmdBufPhys + CMD_RESP_OFF;
    g_descTable[d1].len   = respCap;
    g_descTable[d1].flags = VIRTQ_DESC_F_WRITE;
    g_descTable[d1].next  = 0;

    __asm__ volatile("mfence" ::: "memory");
    g_availRing[g_availIdxShadow % g_queueSize] = head;
    __asm__ volatile("mfence" ::: "memory");
    *g_availIdx = ++g_availIdxShadow;
    __asm__ volatile("mfence" ::: "memory");

    NotifyControlQueue();

    // Bounded spin for completion.
    for (uint64_t i = 0; i < 200000000ull; ++i)
    {
        if (*g_usedIdx != g_usedIdxShadow)
        {
            __asm__ volatile("mfence" ::: "memory");
            uint16_t slot = g_usedIdxShadow % g_queueSize;
            uint32_t len  = g_usedRing[slot].len;
            ++g_usedIdxShadow;
            return len;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    SerialPuts("virtio_gpu: command timeout\n");
    return 0;
}

// ---------------------------------------------------------------------------
// GET_DISPLAY_INFO
// ---------------------------------------------------------------------------

static bool QueryDisplayInfo()
{
    memset(g_cmdBuf, 0, 4096);
    auto* req = reinterpret_cast<VirtioGpuCtrlHdr*>(g_cmdBuf + CMD_REQ_OFF);
    req->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    uint32_t respLen = SubmitCommand(sizeof(VirtioGpuCtrlHdr),
                                     sizeof(VirtioGpuRespDisplayInfo));
    if (respLen < sizeof(VirtioGpuCtrlHdr))
    {
        SerialPrintf("virtio_gpu: GET_DISPLAY_INFO short response (%u)\n", respLen);
        return false;
    }

    auto* resp = reinterpret_cast<VirtioGpuRespDisplayInfo*>(g_cmdBuf + CMD_RESP_OFF);
    if (resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
    {
        SerialPrintf("virtio_gpu: GET_DISPLAY_INFO bad resp type 0x%x\n", resp->hdr.type);
        return false;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; ++i)
    {
        if (!resp->pmodes[i].enabled) continue;
        SerialPrintf("virtio_gpu: scanout %u enabled %ux%u\n",
                     i, resp->pmodes[i].r.width, resp->pmodes[i].r.height);
        if (g_dispWidth == 0)
        {
            g_dispWidth  = resp->pmodes[i].r.width;
            g_dispHeight = resp->pmodes[i].r.height;
        }
    }
    return g_dispWidth != 0;
}

// ---------------------------------------------------------------------------
// GET_CAPSET_INFO — enumerate host 3D capsets (Phase A).
// Only meaningful once VIRGL is negotiated; on a 2D device num_capsets==0 and
// this is skipped. Logs each capset and records whether Venus is available,
// which the later DRM-shim / Venus bring-up (Phases B–C) depends on.
//
// BRO-182 (FIXED): GET_CAPSET_INFO previously came back as RESP_OK_NODATA
// because the command constant was off by one — 0x0107 is actually
// RESOURCE_DETACH_BACKING (which the host answers with OK_NODATA), not
// GET_CAPSET_INFO. Counting the virtio_gpu_ctrl_type enum from 0x0100,
// GET_CAPSET_INFO is 0x0108 and GET_CAPSET is 0x0109 (0x0107 is the detach op).
// Corrected above; capset enumeration and GET_CAPSET now return real data.
// ---------------------------------------------------------------------------

static const char* CapsetName(uint32_t id)
{
    switch (id)
    {
        case VIRTIO_GPU_CAPSET_VIRGL:        return "virgl";
        case VIRTIO_GPU_CAPSET_VIRGL2:       return "virgl2";
        case VIRTIO_GPU_CAPSET_GFXSTREAM:    return "gfxstream";
        case VIRTIO_GPU_CAPSET_VENUS:        return "venus";
        case VIRTIO_GPU_CAPSET_CROSS_DOMAIN: return "cross-domain";
        case VIRTIO_GPU_CAPSET_DRM:          return "drm-native";
        default:                             return "unknown";
    }
}

static void QueryCapsets()
{
    if (!(g_gpu3dFeatures & VIRTIO_GPU_F_VIRGL))
        return;  // 2D-only device; no 3D capsets to enumerate.

    g_numCapsets = mmio_read32(g_deviceCfg, VIRTIO_GPU_CFG_NUM_CAPSETS);
    SerialPrintf("virtio_gpu: 3D enabled, num_capsets=%u\n", g_numCapsets);

    for (uint32_t i = 0; i < g_numCapsets; ++i)
    {
        memset(g_cmdBuf, 0, 4096);
        auto* req = reinterpret_cast<VirtioGpuGetCapsetInfo*>(g_cmdBuf + CMD_REQ_OFF);
        req->hdr.type     = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        req->capset_index = i;

        uint32_t respLen = SubmitCommand(sizeof(VirtioGpuGetCapsetInfo),
                                         sizeof(VirtioGpuRespCapsetInfo));
        auto* resp = reinterpret_cast<VirtioGpuRespCapsetInfo*>(g_cmdBuf + CMD_RESP_OFF);
        if (respLen < sizeof(VirtioGpuRespCapsetInfo) ||
            resp->hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO)
        {
            SerialPrintf("virtio_gpu: GET_CAPSET_INFO[%u] failed (len=%u type=0x%x)\n",
                         i, respLen, resp->hdr.type);
            continue;
        }

        SerialPrintf("virtio_gpu: capset[%u] id=%u (%s) max_version=%u max_size=%u\n",
                     i, resp->capset_id, CapsetName(resp->capset_id),
                     resp->capset_max_version, resp->capset_max_size);

        if (resp->capset_id == VIRTIO_GPU_CAPSET_VENUS &&
            resp->capset_max_version > 0)
            g_haveVenusCapset = true;
    }

    if (g_haveVenusCapset)
        SerialPuts("virtio_gpu: Venus capset present — Vulkan transport available\n");
}

// Fetch a capset blob (virgl_caps) from the host by id+version via the
// VIRTIO_GPU_CMD_GET_CAPSET command. Copies up to maxBytes of the returned blob
// into `out`. Returns bytes written (>0) or <0 on failure. Used by the DRM
// render-node GET_CAPS ioctl so unmodified Mesa can bind the hardware virgl
// screen (otherwise it falls back to software/llvmpipe).
static int32_t GetCapsetBlob(uint32_t capsetId, uint32_t version,
                             void* out, uint32_t maxBytes)
{
    if (!out || maxBytes == 0) return -1;
    memset(g_cmdBuf, 0, 4096);
    auto* req = reinterpret_cast<VirtioGpuGetCapset*>(g_cmdBuf + CMD_REQ_OFF);
    req->hdr.type      = VIRTIO_GPU_CMD_GET_CAPSET;
    req->capset_id     = capsetId;
    req->capset_version = version;

    uint32_t respLen = SubmitCommand(sizeof(VirtioGpuGetCapset), CMD_RESP_CAP);
    auto* resp = reinterpret_cast<VirtioGpuRespCapsetInfo*>(g_cmdBuf + CMD_RESP_OFF);
    if (respLen <= sizeof(VirtioGpuCtrlHdr) ||
        reinterpret_cast<VirtioGpuCtrlHdr*>(resp)->type != VIRTIO_GPU_RESP_OK_CAPSET)
    {
        SerialPrintf("virtio_gpu: GET_CAPSET(id=%u ver=%u) failed (len=%u type=0x%x)\n",
                     capsetId, version, respLen,
                     reinterpret_cast<VirtioGpuCtrlHdr*>(resp)->type);
        return -1;
    }

    uint32_t blobLen = respLen - static_cast<uint32_t>(sizeof(VirtioGpuCtrlHdr));
    if (blobLen > maxBytes) blobLen = maxBytes;
    const uint8_t* blob = g_cmdBuf + CMD_RESP_OFF + sizeof(VirtioGpuCtrlHdr);
    memcpy(out, blob, blobLen);
    SerialPrintf("virtio_gpu: GET_CAPSET(id=%u ver=%u) -> %u bytes\n",
                 capsetId, version, blobLen);
    return static_cast<int32_t>(blobLen);
}

// checks the device returned RESP_OK_NODATA.
// ---------------------------------------------------------------------------

static bool CmdRespOk(uint32_t respLen)
{
    if (respLen < sizeof(VirtioGpuCtrlHdr)) return false;
    auto* resp = reinterpret_cast<VirtioGpuCtrlHdr*>(g_cmdBuf + CMD_RESP_OFF);
    return resp->type == VIRTIO_GPU_RESP_OK_NODATA;
}

static bool ResourceCreate2D(uint32_t resId, uint32_t format, uint32_t w, uint32_t h)
{
    auto* req = reinterpret_cast<VirtioGpuResourceCreate2D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req->resource_id = resId;
    req->format      = format;
    req->width       = w;
    req->height      = h;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

// Attach a physically-contiguous backing buffer as a single mem-entry. The
// framebuffer is allocated via PmmAllocPages (contiguous) so one entry suffices
// — and we must NOT walk it with VmmVirtToPhys, since the direct map is
// huge-page-mapped and the 4K-PTE walker returns 0 there.
static bool ResourceAttachBackingContig(uint32_t resId, uint64_t phys, uint32_t sizeBytes)
{
    auto* req = reinterpret_cast<VirtioGpuResourceAttachBacking*>(g_cmdBuf + CMD_REQ_OFF);
    auto* entries = reinterpret_cast<VirtioGpuMemEntry*>(
        g_cmdBuf + CMD_REQ_OFF + sizeof(VirtioGpuResourceAttachBacking));

    entries[0].addr    = phys;
    entries[0].length  = sizeBytes;
    entries[0].padding = 0;

    memset(&req->hdr, 0, sizeof(req->hdr));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req->resource_id = resId;
    req->nr_entries  = 1;

    SerialPrintf("virtio_gpu: attach backing res %u — contiguous %u KB at 0x%lx\n",
                 resId, sizeBytes / 1024, phys);

    uint32_t reqLen = sizeof(VirtioGpuResourceAttachBacking) + sizeof(VirtioGpuMemEntry);
    return CmdRespOk(SubmitCommand(reqLen, CMD_RESP_CAP));
}

// Attach a backing for a buffer that is virtually contiguous in kernel space but
// physically scattered (e.g. a window VFB from VmmAllocPages). Walks the PTEs
// page-by-page (VmmVirtToPhys works here — these are 4K-mapped, not the
// huge-page direct map), coalescing physically-contiguous runs into as few
// mem-entries as possible. Returns false on any unmapped page.
static bool ResourceAttachBackingVirt(uint32_t resId, uint64_t vaddr, uint32_t sizeBytes)
{
    auto* req = reinterpret_cast<VirtioGpuResourceAttachBacking*>(g_cmdBuf + CMD_REQ_OFF);
    auto* entries = reinterpret_cast<VirtioGpuMemEntry*>(
        g_cmdBuf + CMD_REQ_OFF + sizeof(VirtioGpuResourceAttachBacking));

    // Cap on mem-entries that fit in the request region (pages 0..CMD_PAGES-2).
    const uint32_t maxEntries =
        ((CMD_PAGES - 1) * 4096 - sizeof(VirtioGpuResourceAttachBacking))
        / sizeof(VirtioGpuMemEntry);

    uint32_t nEntries = 0;
    uint64_t base = vaddr & ~0xFFFull;
    uint32_t firstOff = static_cast<uint32_t>(vaddr & 0xFFFull);
    uint32_t remaining = sizeBytes;
    uint64_t va = base;
    uint64_t runPhys = 0;
    uint32_t runLen = 0;

    while (remaining > 0)
    {
        uint64_t phys = VmmVirtToPhys(KernelPageTable, VirtualAddress(va)).raw();
        if (phys == 0)
        {
            SerialPrintf("virtio_gpu: backing walk hit unmapped page at 0x%lx\n", va);
            return false;
        }
        // Bytes contributed by this page (account for an unaligned first page).
        uint32_t pageOff = (va == base) ? firstOff : 0;
        uint32_t chunk = 4096 - pageOff;
        if (chunk > remaining) chunk = remaining;
        uint64_t physStart = phys + pageOff;

        if (runLen != 0 && physStart == runPhys + runLen)
        {
            runLen += chunk;   // extend the contiguous run
        }
        else
        {
            if (runLen != 0)
            {
                if (nEntries >= maxEntries)
                { SerialPuts("virtio_gpu: backing too fragmented\n"); return false; }
                entries[nEntries].addr    = runPhys;
                entries[nEntries].length  = runLen;
                entries[nEntries].padding = 0;
                ++nEntries;
            }
            runPhys = physStart;
            runLen  = chunk;
        }
        remaining -= chunk;
        va += 4096;
    }
    if (runLen != 0)
    {
        if (nEntries >= maxEntries)
        { SerialPuts("virtio_gpu: backing too fragmented\n"); return false; }
        entries[nEntries].addr    = runPhys;
        entries[nEntries].length  = runLen;
        entries[nEntries].padding = 0;
        ++nEntries;
    }

    memset(&req->hdr, 0, sizeof(req->hdr));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req->resource_id = resId;
    req->nr_entries  = nEntries;

    uint32_t reqLen = sizeof(VirtioGpuResourceAttachBacking)
                    + nEntries * sizeof(VirtioGpuMemEntry);
    return CmdRespOk(SubmitCommand(reqLen, CMD_RESP_CAP));
}

static bool SetScanout(uint32_t scanoutId, uint32_t resId, uint32_t w, uint32_t h)
{
    auto* req = reinterpret_cast<VirtioGpuSetScanout*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type     = VIRTIO_GPU_CMD_SET_SCANOUT;
    req->r.width      = w;
    req->r.height     = h;
    req->scanout_id   = scanoutId;
    req->resource_id  = resId;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

static bool TransferToHost2D(uint32_t resId, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h, uint64_t offsetBytes)
{
    auto* req = reinterpret_cast<VirtioGpuTransferToHost2D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    req->r.x = x; req->r.y = y; req->r.width = w; req->r.height = h;
    req->offset      = offsetBytes;
    req->resource_id = resId;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

static bool ResourceFlush(uint32_t resId, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    auto* req = reinterpret_cast<VirtioGpuResourceFlush*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    req->r.x = x; req->r.y = y; req->r.width = w; req->r.height = h;
    req->resource_id = resId;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

// ---------------------------------------------------------------------------
// DisplayOps integration (primary-display path)
// ---------------------------------------------------------------------------

// Push a dirty scanline span [minY, maxY) of the framebuffer to the device.
// Called from the compositor flip via DisplayFlush. The compositor renders into
// g_fbBacking; here we transfer just the damaged rows to the host resource and
// flush them to the scanout. Runs on the compositor thread (never an ISR), so
// the synchronous controlq poll in SubmitCommand is safe.
static void VirtioGpuFlush(uint32_t minY, uint32_t maxY)
{
    if (!g_fbBacking || maxY <= minY) return;
    if (maxY > g_fbH) maxY = g_fbH;
    uint32_t rows   = maxY - minY;
    uint64_t offset = static_cast<uint64_t>(minY) * g_fbW * 4;
    if (!TransferToHost2D(RESOURCE_FB, 0, minY, g_fbW, rows, offset)) return;
    ResourceFlush(RESOURCE_FB, 0, minY, g_fbW, rows);
}

static bool VgpuSetMode(uint32_t /*w*/, uint32_t /*h*/) { return false; } // Phase 6
static void VgpuGetMode(brook::DisplayMode* m)
{
    m->width = g_fbW; m->height = g_fbH; m->stride = g_fbW * 4; m->bpp = 32;
}
static volatile uint32_t* VgpuGetFramebuffer()
{ return reinterpret_cast<volatile uint32_t*>(g_fbBacking); }
static uint64_t VgpuGetFramebufferPhys() { return g_fbBackingPhys; }

static const brook::DisplayOps g_vgpuDisplayOps = {
    "virtio-gpu",
    VgpuSetMode,
    VgpuGetMode,
    VgpuGetFramebuffer,
    VgpuGetFramebufferPhys,
    VirtioGpuFlush,
};

// Take over the display: create a framebuffer resource backed by physically
// contiguous guest RAM, point both the TTY and compositor at that backing (so
// CompositorInit — which runs after module load — adopts it), set scanout, and
// register as the active DisplayOps. After this the compositor composites into
// g_fbBacking and each flip calls VirtioGpuFlush to present the damage rect.
static bool VirtioGpuTakeOverDisplay()
{
    // Adopt the current boot framebuffer resolution (GOP/VGA set by firmware).
    uint32_t* ttyPix; uint32_t w, h, strideBytes;
    if (!TtyGetFramebuffer(&ttyPix, &w, &h, &strideBytes) || w == 0 || h == 0)
    {
        SerialPuts("virtio_gpu: no TTY framebuffer to adopt\n");
        return false;
    }

    uint32_t bytes = w * h * 4;
    uint32_t pages = AlignUp(bytes, 4096) / 4096;

    // Physically contiguous: serves as the device backing AND the compositor's
    // PhysToVirt-mapped front buffer (CompositorRemap assumes contiguous phys).
    PhysicalAddress phys = PmmAllocPages(pages, MemTag::Device, KernelPid);
    if (!phys) { SerialPuts("virtio_gpu: fb backing alloc failed\n"); return false; }
    g_fbBackingPhys = phys.raw();
    g_fbBacking     = reinterpret_cast<uint8_t*>(PhysToVirt(phys).raw());
    g_fbW = w; g_fbH = h;
    memset(g_fbBacking, 0, bytes);

    if (!ResourceCreate2D(RESOURCE_FB, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, w, h))
    { SerialPuts("virtio_gpu: RESOURCE_CREATE_2D failed\n"); return false; }
    if (!ResourceAttachBackingContig(RESOURCE_FB, g_fbBackingPhys, bytes))
    { SerialPuts("virtio_gpu: ATTACH_BACKING failed\n"); return false; }
    if (!SetScanout(0, RESOURCE_FB, w, h))
    { SerialPuts("virtio_gpu: SET_SCANOUT failed\n"); return false; }

    // Redirect TTY + compositor into our backing, then register as the display.
    // stride == width (no row padding) since we allocated exactly w*h*4.
    TtyRemap(g_fbBackingPhys, w, h, w);
    CompositorRemap(g_fbBackingPhys, w, h, w);
    DisplayRegister(&g_vgpuDisplayOps);

    // Present the initial (cleared) frame.
    TransferToHost2D(RESOURCE_FB, 0, 0, w, h, 0);
    ResourceFlush(RESOURCE_FB, 0, 0, w, h);

    KPrintf("virtio_gpu: registered as primary display %ux%u\n", w, h);
    return true;
}

// ---------------------------------------------------------------------------
// VirGL 3D bring-up (milestone 1: shader-free GPU CLEAR self-test).
// ---------------------------------------------------------------------------

static bool CtxCreate(uint32_t ctxId, uint32_t capsetId)
{
    auto* req = reinterpret_cast<VirtioGpuCtxCreate*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type     = VIRTIO_GPU_CMD_CTX_CREATE;
    req->hdr.ctx_id   = ctxId;
    req->context_init = capsetId;   // significant under F_CONTEXT_INIT
    const char* name = "brook3d";
    uint32_t n = 0;
    while (name[n] && n < sizeof(req->debug_name) - 1) { req->debug_name[n] = name[n]; n++; }
    req->nlen = n;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

static bool CtxAttachResource(uint32_t ctxId, uint32_t resId)
{
    auto* req = reinterpret_cast<VirtioGpuCtxResource*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type    = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    req->hdr.ctx_id  = ctxId;
    req->resource_id = resId;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

static bool ResourceCreate3D(uint32_t resId, uint32_t format, uint32_t bind,
                             uint32_t w, uint32_t h)
{
    auto* req = reinterpret_cast<VirtioGpuResourceCreate3D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    req->resource_id = resId;
    req->target      = PIPE_TEXTURE_2D;
    req->format      = format;
    req->bind        = bind;
    req->width       = w;
    req->height      = h;
    req->depth       = 1;
    req->array_size  = 1;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

static bool TransferFromHost3D(uint32_t ctxId, uint32_t resId, uint32_t w, uint32_t h)
{
    auto* req = reinterpret_cast<VirtioGpuTransferHost3D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type     = VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    req->hdr.ctx_id   = ctxId;
    req->box.w        = w;
    req->box.h        = h;
    req->box.d        = 1;
    req->resource_id  = resId;
    req->stride       = w * 4;
    req->layer_stride = w * h * 4;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

// Upload a dirty rect (x,y,w,h) from a resource's guest backing into the host
// texture. `texW`/`texH` are the full texture dimensions (row stride + layer
// size). Used each frame to push changed window pixels host-side via device DMA
// (no CPU framebuffer copy).
static bool TransferToHost3D(uint32_t ctxId, uint32_t resId,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t texW, uint32_t texH)
{
    auto* req = reinterpret_cast<VirtioGpuTransferHost3D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type     = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    req->hdr.ctx_id   = ctxId;
    req->box.x        = x;
    req->box.y        = y;
    req->box.w        = w;
    req->box.h        = h;
    req->box.d        = 1;
    req->offset       = static_cast<uint64_t>(y) * texW * 4 + static_cast<uint64_t>(x) * 4;
    req->resource_id  = resId;
    req->stride       = texW * 4;          // full row stride
    req->layer_stride = texW * texH * 4;   // full layer size
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}


// must be unique per resource within the context (object id namespace).
static bool Submit3DClear(uint32_t ctxId, uint32_t resId, uint32_t surfHandle,
                          float r, float g, float b, float a)
{
    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
    memset(sub, 0, sizeof(*sub));
    sub->hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
    sub->hdr.ctx_id = ctxId;

    uint32_t* dw = reinterpret_cast<uint32_t*>(
        g_cmdBuf + CMD_REQ_OFF + sizeof(VirtioGpuCmdSubmit));
    uint32_t n = 0;

    // CREATE_OBJECT SURFACE (payload 5 dwords) over the render-target texture.
    dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    dw[n++] = surfHandle;
    dw[n++] = resId;
    dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    dw[n++] = 0;   // texture level
    dw[n++] = 0;   // texture layers (first | last << 16)

    // SET_FRAMEBUFFER_STATE (payload nr_cbufs+2 = 3): 1 colour buffer, no zsurf.
    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    dw[n++] = 1;            // nr_cbufs
    dw[n++] = 0;            // zsurf handle
    dw[n++] = surfHandle;   // cbuf[0]

    // CLEAR (payload 8): colour0 only; depth (double) + stencil zeroed.
    dw[n++] = VirglCmd0(VIRGL_CCMD_CLEAR, 0, 8);
    dw[n++] = PIPE_CLEAR_COLOR0;
    dw[n++] = F32Bits(r); dw[n++] = F32Bits(g);
    dw[n++] = F32Bits(b); dw[n++] = F32Bits(a);
    dw[n++] = 0; dw[n++] = 0;   // depth
    dw[n++] = 0;                // stencil

    sub->size = n * 4;
    return CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP));
}

// Build + submit a virgl BLIT that copies a srcW×srcH region at (srcX,srcY) of
// `srcRes` into `dstRes` at (dstX,dstY,dstW,dstH) — the GPU does the copy/scale
// (and optional alpha blend) entirely host-side, no guest pixel touch. This is
// the core compositor primitive: one BLIT per window into the scanout. Nearest
// filter, full RGBA mask.
static bool Submit3DBlit(uint32_t ctxId, uint32_t dstRes,
                         uint32_t dstX, uint32_t dstY, uint32_t dstW, uint32_t dstH,
                         uint32_t srcRes,
                         uint32_t srcX, uint32_t srcY, uint32_t srcW, uint32_t srcH,
                         bool alphaBlend)
{
    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
    memset(sub, 0, sizeof(*sub));
    sub->hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
    sub->hdr.ctx_id = ctxId;

    uint32_t* dw = reinterpret_cast<uint32_t*>(
        g_cmdBuf + CMD_REQ_OFF + sizeof(VirtioGpuCmdSubmit));
    uint32_t n = 0;

    // BLIT (payload 21 dwords). S0 packs mask|filter|...|alpha_blend.
    uint32_t s0 = (VIRGL_BLIT_MASK_RGBA & 0xFF)
                | ((VIRGL_TEX_FILTER_NEAREST & 0x3) << 8)
                | ((alphaBlend ? 1u : 0u) << 12);
    dw[n++] = VirglCmd0(VIRGL_CCMD_BLIT, 0, 21);
    dw[n++] = s0;
    dw[n++] = 0;   // scissor minx|miny (disabled)
    dw[n++] = 0;   // scissor maxx|maxy
    dw[n++] = dstRes;
    dw[n++] = 0;                              // dst level
    dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;    // dst format
    dw[n++] = dstX; dw[n++] = dstY; dw[n++] = 0;          // dst x,y,z
    dw[n++] = dstW; dw[n++] = dstH; dw[n++] = 1;          // dst w,h,d
    dw[n++] = srcRes;
    dw[n++] = 0;                              // src level
    dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;    // src format
    dw[n++] = srcX; dw[n++] = srcY; dw[n++] = 0;          // src x,y,z
    dw[n++] = srcW; dw[n++] = srcH; dw[n++] = 1;          // src w,h,d

    sub->size = n * 4;
    return CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP));
}

// Shader-free GPU self-test in two stages, proving the primitives the GPU
// compositor is built on:
//   (1) CLEAR  — create a 3D context + render-target texture, CLEAR it green via
//       SUBMIT_3D, transfer it back, verify. Proves ctx/resource/submit/transfer.
//   (2) BLIT   — clear a second target blue, then BLIT a 32x32 region of the
//       green texture into it at (16,16); transfer back and verify a green
//       square on a blue field. Proves the GPU-side rect copy that backs the
//       compositor (one BLIT per window, no guest pixel touch, no shaders).
// All verifiable over serial (no GL screendump needed). Lights the taskbar 3D
// badge once both stages pass.
static void VirtioGpu3DSelfTest()
{
    if (!(g_gpu3dFeatures & VIRTIO_GPU_F_VIRGL))
        return;   // 2D device — nothing to test.

    const uint32_t dim   = SELFTEST_DIM;
    const uint32_t bytes = dim * dim * 4;
    const uint32_t pages = AlignUp(bytes, 4096) / 4096;

    PhysicalAddress phys = PmmAllocPages(pages, MemTag::Device, KernelPid);
    if (!phys) { SerialPuts("virtio_gpu: 3D self-test backing alloc failed\n"); return; }
    uint32_t* readback = reinterpret_cast<uint32_t*>(PhysToVirt(phys).raw());
    memset(readback, 0, bytes);

    if (!CtxCreate(CTX_ID_SELFTEST, VIRTIO_GPU_CAPSET_VIRGL))
    { SerialPuts("virtio_gpu: 3D self-test CTX_CREATE failed\n"); return; }
    if (!ResourceCreate3D(RES_3D_SELFTEST, VIRGL_FORMAT_B8G8R8X8_UNORM,
                          VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW, dim, dim))
    { SerialPuts("virtio_gpu: 3D self-test RESOURCE_CREATE_3D failed\n"); return; }
    if (!ResourceAttachBackingContig(RES_3D_SELFTEST, phys.raw(), bytes))
    { SerialPuts("virtio_gpu: 3D self-test ATTACH_BACKING failed\n"); return; }
    if (!CtxAttachResource(CTX_ID_SELFTEST, RES_3D_SELFTEST))
    { SerialPuts("virtio_gpu: 3D self-test CTX_ATTACH_RESOURCE failed\n"); return; }

    // --- Stage 1: CLEAR src to opaque green (R=0, G=1, B=0, A=1). ---
    if (!Submit3DClear(CTX_ID_SELFTEST, RES_3D_SELFTEST, SURF_SRC, 0.0f, 1.0f, 0.0f, 1.0f))
    { SerialPuts("virtio_gpu: 3D self-test SUBMIT_3D(clear) failed\n"); return; }
    if (!TransferFromHost3D(CTX_ID_SELFTEST, RES_3D_SELFTEST, dim, dim))
    { SerialPuts("virtio_gpu: 3D self-test TRANSFER_FROM_HOST_3D failed\n"); return; }

    // B8G8R8X8 green readback = bytes [B=0, G=0xFF, R=0, X] = 0x0000FF00 LE.
    uint32_t px = readback[0];
    auto chanB = [](uint32_t p){ return p & 0xFF; };
    auto chanG = [](uint32_t p){ return (p >> 8) & 0xFF; };
    auto chanR = [](uint32_t p){ return (p >> 16) & 0xFF; };
    bool clearGreen = (chanG(px) > 0xC0) && (chanR(px) < 0x40) && (chanB(px) < 0x40);
    SerialPrintf("virtio_gpu: 3D self-test [clear] px[0]=0x%08x (r=%u g=%u b=%u) -> %s\n",
                 px, chanR(px), chanG(px), chanB(px), clearGreen ? "PASS" : "FAIL");
    if (!clearGreen) return;

    // --- Stage 2: BLIT a 32x32 corner of the green texture onto a blue target. ---
    PhysicalAddress phys2 = PmmAllocPages(pages, MemTag::Device, KernelPid);
    if (!phys2) { SerialPuts("virtio_gpu: 3D self-test dst backing alloc failed\n"); return; }
    uint32_t* readback2 = reinterpret_cast<uint32_t*>(PhysToVirt(phys2).raw());
    memset(readback2, 0, bytes);

    if (!ResourceCreate3D(RES_3D_BLITDST, VIRGL_FORMAT_B8G8R8X8_UNORM,
                          VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW, dim, dim))
    { SerialPuts("virtio_gpu: 3D self-test BLITDST create failed\n"); return; }
    if (!ResourceAttachBackingContig(RES_3D_BLITDST, phys2.raw(), bytes))
    { SerialPuts("virtio_gpu: 3D self-test BLITDST attach failed\n"); return; }
    if (!CtxAttachResource(CTX_ID_SELFTEST, RES_3D_BLITDST))
    { SerialPuts("virtio_gpu: 3D self-test BLITDST ctx-attach failed\n"); return; }

    // Clear dst to opaque blue (R=0, G=0, B=1, A=1).
    if (!Submit3DClear(CTX_ID_SELFTEST, RES_3D_BLITDST, SURF_DST, 0.0f, 0.0f, 1.0f, 1.0f))
    { SerialPuts("virtio_gpu: 3D self-test dst clear failed\n"); return; }
    // BLIT green[0,0..32,32] -> dst[16,16..48,48].
    if (!Submit3DBlit(CTX_ID_SELFTEST, RES_3D_BLITDST, 16, 16, 32, 32,
                      RES_3D_SELFTEST, 0, 0, 32, 32, /*alphaBlend=*/false))
    { SerialPuts("virtio_gpu: 3D self-test SUBMIT_3D(blit) failed\n"); return; }
    if (!TransferFromHost3D(CTX_ID_SELFTEST, RES_3D_BLITDST, dim, dim))
    { SerialPuts("virtio_gpu: 3D self-test blit readback failed\n"); return; }

    // Verify: outside the square is blue; centre of the square is green.
    uint32_t corner = readback2[0 * dim + 0];          // (0,0): blue
    uint32_t centre = readback2[32 * dim + 32];        // (32,32): inside 16..48 → green
    bool cornerBlue = (chanB(corner) > 0xC0) && (chanR(corner) < 0x40) && (chanG(corner) < 0x40);
    bool centreGreen = (chanG(centre) > 0xC0) && (chanR(centre) < 0x40) && (chanB(centre) < 0x40);
    bool blitOk = cornerBlue && centreGreen;
    SerialPrintf("virtio_gpu: 3D self-test [blit] corner=0x%08x centre=0x%08x -> %s\n",
                 corner, centre, blitOk ? "PASS" : "FAIL");
    if (!blitOk) return;

    // --- Gate #1: scanout-as-3D-RT present path ---
    // The GPU compositor composes into a 3D render-target and presents it via
    // SET_SCANOUT + FLUSH (no guest framebuffer copy). Prove that path here:
    // create a scanout-capable 3D RT, SET_SCANOUT it, CLEAR it red, BLIT the
    // green texture's corner into it, FLUSH (present), then read it back and
    // verify. A backing is attached only so this test can read the result; the
    // real compositor scanout needs none. Runs before display takeover, which
    // re-points scanout 0 at the 2D framebuffer afterwards.
    {
        const uint32_t sdim  = SCANOUT_TEST_DIM;
        const uint32_t sbytes = sdim * sdim * 4;
        const uint32_t spages = AlignUp(sbytes, 4096) / 4096;
        PhysicalAddress sphys = PmmAllocPages(spages, MemTag::Device, KernelPid);
        if (!sphys) { SerialPuts("virtio_gpu: 3D self-test scanout alloc failed\n"); return; }
        uint32_t* sread = reinterpret_cast<uint32_t*>(PhysToVirt(sphys).raw());
        memset(sread, 0, sbytes);

        if (!ResourceCreate3D(RES_3D_SCANOUT, VIRGL_FORMAT_B8G8R8X8_UNORM,
                              VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SCANOUT |
                              VIRGL_BIND_SAMPLER_VIEW, sdim, sdim))
        { SerialPuts("virtio_gpu: 3D self-test scanout RT create failed\n"); return; }
        if (!ResourceAttachBackingContig(RES_3D_SCANOUT, sphys.raw(), sbytes))
        { SerialPuts("virtio_gpu: 3D self-test scanout backing failed\n"); return; }
        if (!CtxAttachResource(CTX_ID_SELFTEST, RES_3D_SCANOUT))
        { SerialPuts("virtio_gpu: 3D self-test scanout ctx-attach failed\n"); return; }

        bool scanoutSet = SetScanout(0, RES_3D_SCANOUT, sdim, sdim);
        SerialPrintf("virtio_gpu: 3D self-test [scanout] SET_SCANOUT(3D RT) -> %s\n",
                     scanoutSet ? "OK" : "REJECTED");

        // Compose: clear red, then BLIT a 32x32 green corner to (64,64).
        if (!Submit3DClear(CTX_ID_SELFTEST, RES_3D_SCANOUT, SURF_SCANOUT,
                           1.0f, 0.0f, 0.0f, 1.0f))
        { SerialPuts("virtio_gpu: 3D self-test scanout clear failed\n"); return; }
        if (!Submit3DBlit(CTX_ID_SELFTEST, RES_3D_SCANOUT, 64, 64, 32, 32,
                          RES_3D_SELFTEST, 0, 0, 32, 32, /*alphaBlend=*/false))
        { SerialPuts("virtio_gpu: 3D self-test scanout blit failed\n"); return; }

        // Present (the op the compositor calls each frame).
        bool flushed = ResourceFlush(RES_3D_SCANOUT, 0, 0, sdim, sdim);

        // Read back and verify the composed scanout content.
        if (!TransferFromHost3D(CTX_ID_SELFTEST, RES_3D_SCANOUT, sdim, sdim))
        { SerialPuts("virtio_gpu: 3D self-test scanout readback failed\n"); return; }
        uint32_t sBg = sread[0 * sdim + 0];       // (0,0): red
        uint32_t sFg = sread[80 * sdim + 80];     // (80,80): inside 64..96 → green
        bool bgRed   = (chanR(sBg) > 0xC0) && (chanG(sBg) < 0x40) && (chanB(sBg) < 0x40);
        bool fgGreen = (chanG(sFg) > 0xC0) && (chanR(sFg) < 0x40) && (chanB(sFg) < 0x40);
        bool scanoutOk = scanoutSet && flushed && bgRed && fgGreen;
        SerialPrintf("virtio_gpu: 3D self-test [scanout] bg=0x%08x fg=0x%08x flush=%d -> %s\n",
                     sBg, sFg, flushed ? 1 : 0, scanoutOk ? "PASS" : "FAIL");
        g_gpuScanoutRtOk = scanoutOk;
    }

    g_gpu3dWorks = true;
    DisplaySet3DActive(true);
    KPrintf("virtio_gpu: host 3D (virgl) clear+blit compositor primitives confirmed live\n");
}


// ===========================================================================
// GPU compositor: BLIT-based window composition.
//
// The window compositor composes window content on the GPU via these ops
// (registered as GpuCompositorOps). Each window's pixel buffer becomes a host
// sampler-view texture backed by the window's VFB pages; one BLIT per window
// draws it into a persistent scanout render-target, which is then presented
// with RESOURCE_FLUSH. The CPU never blits window pixels into the framebuffer.
// ===========================================================================

static constexpr uint32_t COMP_CTX_ID       = 2;     // separate from self-test ctx 1
static constexpr uint32_t COMP_SCANOUT_RES  = 16;    // scanout render-target
static constexpr uint32_t COMP_SCANOUT_SURF = 1;     // virgl surface object handle
static constexpr uint32_t COMP_THUMB_RES    = 15;    // downscale thumbnail RT
static constexpr uint32_t COMP_FULL_RES     = 14;    // full-res 1:1 readback RT (lazy)
static constexpr uint32_t COMP_THUMB_W      = 256;   // thumbnail size (16:9-ish)
static constexpr uint32_t COMP_THUMB_H      = 144;
static constexpr uint32_t COMP_FIRST_TEX_RES = 17;   // texture resource ids from here

static bool     g_compReady       = false;   // ops set up + scanout RT created
static bool     g_compScanoutBound = false;  // SET_SCANOUT to the RT has happened
static uint32_t g_compScanoutW    = 0;
static uint32_t g_compScanoutH    = 0;
static uint32_t g_compNextRes     = COMP_FIRST_TEX_RES;
static uint32_t g_composeN        = 0;       // dwords accumulated in compose stream
static uint64_t g_compThumbPhys   = 0;       // thumbnail RT readback backing (phys)
static uint32_t* g_compThumbBuf   = nullptr; // thumbnail RT readback backing (virt)
static uint32_t* g_compFullBuf    = nullptr; // full-res RT readback backing (virt, lazy)
static bool      g_compFullReady  = false;   // full-res readback RT created

// --- DRAW (textured-quad) composition path --------------------------------
// The GPU compose path: each layer is drawn as a textured quad through the real
// GL pipeline (vertex+fragment TGSI shaders, blend), which enables per-window
// opacity and generalises to Venus/Vulkan. The pipeline objects are set up when
// 3D is available; the compositor uses this path whenever DrawSupported() is
// true (else the CPU compositor path is used). Proven in isolation by the DRAW
// self-test ladder (M1-M5).
static bool      g_drawReady     = false;    // persistent draw objects created
static uint32_t* g_drawVtxBuf    = nullptr;  // per-frame vertex staging (mapped backing)
static uint64_t  g_drawVtxPhys   = 0;        // its physical address
// Persistent draw-pipeline object handles in COMP_CTX_ID (surface=1 already used).
static constexpr uint32_t COMP_DRAW_VBUF_RES = 13;   // dynamic vertex buffer resource
static constexpr uint32_t DRAW_BLEND = 2, DRAW_BLEND_ON = 3, DRAW_RAST = 4,
                          DRAW_DSA = 5, DRAW_VE = 6, DRAW_SAMP = 7,
                          DRAW_VS = 8, DRAW_FS = 9;
// Frosted-glass backdrop blur: fixed-size downsampled RTs + separable gaussian.
// Fixed dims (independent of display size) keep the gaussian texel offsets as
// compile-time constants; the downsample BLIT handles any scanout resolution and
// the backdrop quad samples in normalized [0,1] uv, so this works at any size.
static constexpr uint32_t COMP_BLUR_W   = 480;   // ~1/4 of 1920
static constexpr uint32_t COMP_BLUR_H   = 270;   // ~1/4 of 1080
static constexpr uint32_t COMP_BLUR_A_RES   = 5;    // downsample + final blurred RT
static constexpr uint32_t COMP_BLUR_B_RES   = 6;    // ping-pong temp RT
static constexpr uint32_t COMP_BLURVTX_RES  = 7;    // full-screen blur-pass vertex buffer
static constexpr uint32_t DRAW_FS_BLURH = 10, DRAW_FS_BLURV = 11, DRAW_SAMP_LIN = 12,
                          BLUR_A_SURF = 13, BLUR_B_SURF = 14;
static uint32_t  g_drawNextSview = 256;      // monotonic sampler-view handle allocator
static bool      g_blurReady     = false;    // blur RTs/shaders/sampler set up
static uint32_t  g_blurAView     = 0;        // sampler view for COMP_BLUR_A_RES
static uint32_t  g_blurBView     = 0;        // sampler view for COMP_BLUR_B_RES
static uint64_t  g_blurVtxPhys   = 0;
static uint32_t* g_blurVtxBuf    = nullptr;
// Backdrop blur barriers. Each barrier records a quad index at which the scanout
// (everything drawn so far = everything below the corresponding glass window) is
// snapshotted + blurred into BLUR_A; the glass window's following backdrop quad
// then samples it. Supporting MULTIPLE barriers per frame lets the compositor
// composite glass windows in a single strict back-to-front pass, so each glass
// window frosts exactly what is beneath it (incl. lower glass windows) — correct
// occlusion/blend order for 3+ interleaved or stacked glass windows (BRO-185).
static constexpr uint32_t MAX_BLUR_BARRIERS = 16;
static uint32_t  g_blurBarrierIdx[MAX_BLUR_BARRIERS] = {};      // quad index per barrier (ascending)
static uint32_t  g_blurBarrierStrength[MAX_BLUR_BARRIERS] = {}; // app blur amount per barrier
static uint32_t  g_blurBarrierCount = 0;                        // active barriers this frame
// Quad flags.
static constexpr uint32_t QUAD_SAMPLE_BLUR = 1u << 0;  // sample the blurred backdrop (sview ignored)
// One recorded quad (filled by CompDrawQuad, consumed by CompEndFrame). Dst is in
// integer screen pixels; uv is precomputed float bits; blend selects the object.
struct DrawQuadRec {
    int32_t  dx, dy, dx2, dy2;   // dst rect (screen px), [dx,dx2) x [dy,dy2)
    uint32_t u0, v0, u1, v1;     // source uv as IEEE-754 bits
    uint32_t opacity;            // per-window opacity in [0,1] as IEEE-754 bits
    uint32_t sview;              // texture sampler-view handle
    uint32_t blend;             // DRAW_BLEND (opaque) or DRAW_BLEND_ON (alpha)
    uint32_t flags;             // QUAD_* bits
};
static constexpr uint32_t MAX_DRAW_QUADS = 256;
static DrawQuadRec g_drawQuads[MAX_DRAW_QUADS];
static uint32_t    g_drawQuadCount = 0;

struct GpuTexture { uint32_t resId; uint32_t w; uint32_t h; uint32_t format; bool used; uint32_t sview; };
static constexpr uint32_t MAX_GPU_TEXTURES = 128;
static GpuTexture g_gpuTextures[MAX_GPU_TEXTURES] = {};

static inline uint32_t* ComposeDwBase()
{ return reinterpret_cast<uint32_t*>(g_cmdBuf + CMD_REQ_OFF + sizeof(VirtioGpuCmdSubmit)); }

// Forward declarations for the DRAW-path helpers (defined with the DRAW
// self-test further down) so the compositor's draw setup can use them.
static bool ResourceCreateBuffer(uint32_t resId, uint32_t bind, uint32_t byteLen);
static bool TransferToHostBuffer(uint32_t ctxId, uint32_t resId, uint32_t byteLen);
static bool CreateShaderObj(uint32_t ctxId, uint32_t handle, uint32_t shaderType,
                            const char* text);

// Pass-through vertex shader (clip-space pos from IN[0], uv from IN[1]) and a
// textured fragment shader (sample SVIEW[0] at the interpolated uv). Used by the
// DRAW self-test. Sent as TGSI text.
static const char* kDrawVS =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "MOV OUT[0], IN[0]\n"
    "MOV OUT[1], IN[1]\n"
    "END\n";
static const char* kDrawFS_Tex =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "TEX OUT[0], IN[0], SAMP[0], 2D\n"
    "END\n";

// Compositor DRAW shaders. The vertex carries a third attribute (IN[2].x) — the
// per-window opacity in [0,1] — passed through to the fragment stage, which
// samples the window texture and multiplies the sampled alpha by it. With
// src-alpha-over blending this yields proper per-window opacity (BLIT cannot).
// For opaque layers the compositor binds the no-blend state, so the scaled
// alpha is simply ignored and the colour overwrites.
static const char* kCompVS =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL IN[2]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "DCL OUT[2], GENERIC[1]\n"
    "MOV OUT[0], IN[0]\n"
    "MOV OUT[1], IN[1]\n"
    "MOV OUT[2], IN[2]\n"
    "END\n";
static const char* kCompFS =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL IN[1], GENERIC[1], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL TEMP[0]\n"
    "TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "MUL TEMP[0].w, TEMP[0].wwww, IN[1].xxxx\n"
    "MOV OUT[0], TEMP[0]\n"
    "END\n";

// Separable 5-tap gaussian for the backdrop blur. Taps at 0, +-1, +-2 texels
// with weights 0.375 / 0.25 / 0.0625 (sum 1.0). The blur RT is a fixed
// COMP_BLUR_W x COMP_BLUR_H, so the texel offsets are compile-time constants
// (1/480 in x, 1/270 in y). Reuses kCompVS (uv in GENERIC[0]); opacity unused.
static const char* kBlurH =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL TEMP[0]\n"
    "DCL TEMP[1]\n"
    "DCL TEMP[2]\n"
    "IMM[0] FLT32 { 0.00208333, 0.0000, 0.0000, 0.0000 }\n"
    "IMM[1] FLT32 { 0.00416667, 0.0000, 0.0000, 0.0000 }\n"
    "IMM[2] FLT32 { 0.3750, 0.2500, 0.0625, 0.0000 }\n"
    "TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "MUL TEMP[0], TEMP[0], IMM[2].xxxx\n"
    "ADD TEMP[1], IN[0], IMM[0]\n"
    "TEX TEMP[2], TEMP[1], SAMP[0], 2D\n"
    "MAD TEMP[0], TEMP[2], IMM[2].yyyy, TEMP[0]\n"
    "SUB TEMP[1], IN[0], IMM[0]\n"
    "TEX TEMP[2], TEMP[1], SAMP[0], 2D\n"
    "MAD TEMP[0], TEMP[2], IMM[2].yyyy, TEMP[0]\n"
    "ADD TEMP[1], IN[0], IMM[1]\n"
    "TEX TEMP[2], TEMP[1], SAMP[0], 2D\n"
    "MAD TEMP[0], TEMP[2], IMM[2].zzzz, TEMP[0]\n"
    "SUB TEMP[1], IN[0], IMM[1]\n"
    "TEX TEMP[2], TEMP[1], SAMP[0], 2D\n"
    "MAD TEMP[0], TEMP[2], IMM[2].zzzz, TEMP[0]\n"
    "MOV OUT[0], TEMP[0]\n"
    "END\n";
static const char* kBlurV =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL TEMP[0]\n"
    "DCL TEMP[1]\n"
    "DCL TEMP[2]\n"
    "IMM[0] FLT32 { 0.0000, 0.00370370, 0.0000, 0.0000 }\n"
    "IMM[1] FLT32 { 0.0000, 0.00740741, 0.0000, 0.0000 }\n"
    "IMM[2] FLT32 { 0.3750, 0.2500, 0.0625, 0.0000 }\n"
    "TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "MUL TEMP[0], TEMP[0], IMM[2].xxxx\n"
    "ADD TEMP[1], IN[0], IMM[0]\n"
    "TEX TEMP[2], TEMP[1], SAMP[0], 2D\n"
    "MAD TEMP[0], TEMP[2], IMM[2].yyyy, TEMP[0]\n"
    "SUB TEMP[1], IN[0], IMM[0]\n"
    "TEX TEMP[2], TEMP[1], SAMP[0], 2D\n"
    "MAD TEMP[0], TEMP[2], IMM[2].yyyy, TEMP[0]\n"
    "ADD TEMP[1], IN[0], IMM[1]\n"
    "TEX TEMP[2], TEMP[1], SAMP[0], 2D\n"
    "MAD TEMP[0], TEMP[2], IMM[2].zzzz, TEMP[0]\n"
    "SUB TEMP[1], IN[0], IMM[1]\n"
    "TEX TEMP[2], TEMP[1], SAMP[0], 2D\n"
    "MAD TEMP[0], TEMP[2], IMM[2].zzzz, TEMP[0]\n"
    "MOV OUT[0], TEMP[0]\n"
    "END\n";

// One-time setup of the persistent DRAW pipeline objects in COMP_CTX_ID: a
// dynamic vertex buffer (refilled each frame), two blend states (opaque +
// src-alpha-over), rasterizer, depth/stencil-off, vertex-element layout,
// nearest sampler, and the pass-through + textured shaders. Mirrors the
// proven DRAW self-test. Returns false (leaving the BLIT path active) on any
// failure. Per-texture sampler views are created lazily in CompCreateTexture.
static bool SetupGpuDrawPipeline()
{
    // Dynamic vertex buffer: MAX_DRAW_QUADS quads * 4 verts * 5 floats * 4 bytes
    // (pos.xy, uv.xy, opacity).
    const uint32_t vbytes = MAX_DRAW_QUADS * 4 * 5 * 4;
    PhysicalAddress vp = PmmAllocPages(AlignUp(vbytes, 4096) / 4096, MemTag::Device, KernelPid);
    if (!vp) { SerialPuts("virtio_gpu: draw vbuf alloc failed\n"); return false; }
    g_drawVtxPhys = vp.raw();
    g_drawVtxBuf  = reinterpret_cast<uint32_t*>(PhysToVirt(vp).raw());
    memset(g_drawVtxBuf, 0, vbytes);
    if (!ResourceCreateBuffer(COMP_DRAW_VBUF_RES, VIRGL_BIND_VERTEX_BUFFER, vbytes) ||
        !ResourceAttachBackingContig(COMP_DRAW_VBUF_RES, g_drawVtxPhys, vbytes) ||
        !CtxAttachResource(COMP_CTX_ID, COMP_DRAW_VBUF_RES))
    { SerialPuts("virtio_gpu: draw vbuf resource failed\n"); g_drawVtxBuf = nullptr; return false; }

    // Pipeline state objects, one submit.
    {
        auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
        memset(sub, 0, sizeof(*sub));
        sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = COMP_CTX_ID;
        uint32_t* dw = ComposeDwBase(); uint32_t n = 0;
        // Opaque blend (colormask only).
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11);
        dw[n++] = DRAW_BLEND; dw[n++] = 0; dw[n++] = 0;
        dw[n++] = (0xFu << 27);
        for (int i = 1; i < 8; ++i) dw[n++] = 0;
        // Src-alpha-over blend.
        {
            uint32_t s2 = 1u
                        | (PIPE_BLEND_ADD << 1)
                        | (PIPE_BLENDFACTOR_SRC_ALPHA << 4)
                        | (PIPE_BLENDFACTOR_INV_SRC_ALPHA << 9)
                        | (PIPE_BLEND_ADD << 14)
                        | (PIPE_BLENDFACTOR_SRC_ALPHA << 17)
                        | (PIPE_BLENDFACTOR_INV_SRC_ALPHA << 22)
                        | (0xFu << 27);
            dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11);
            dw[n++] = DRAW_BLEND_ON; dw[n++] = 0; dw[n++] = 0;
            dw[n++] = s2;
            for (int i = 1; i < 8; ++i) dw[n++] = 0;
        }
        // Rasterizer.
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9);
        dw[n++] = DRAW_RAST;
        dw[n++] = (1u << 1) | (1u << 29);    // DEPTH_CLIP | HALF_PIXEL_CENTER
        dw[n++] = F32Bits(1.0f);
        dw[n++] = 0; dw[n++] = 0;
        dw[n++] = F32Bits(1.0f);
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        // DSA off.
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5);
        dw[n++] = DRAW_DSA; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        // Vertex elements: pos @0 (RG32F), uv @8 (RG32F), opacity @16 (R32F), vb 0.
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 13);
        dw[n++] = DRAW_VE;
        dw[n++] = 0;  dw[n++] = 0; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_R32G32_FLOAT;
        dw[n++] = 8;  dw[n++] = 0; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_R32G32_FLOAT;
        dw[n++] = 16; dw[n++] = 0; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_R32_FLOAT;
        // Sampler state: nearest, wrap repeat (all-zero S0).
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_STATE, 9);
        dw[n++] = DRAW_SAMP; dw[n++] = 0;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        sub->size = n * 4;
        if (!CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP)))
        { SerialPuts("virtio_gpu: draw pipeline objects failed\n"); return false; }
    }

    // Shaders.
    // Shaders: the compositor uses the opacity-aware pass-through pair so per-quad
    // opacity (3rd vertex attribute) scales sampled alpha for src-alpha-over blend.
    if (!CreateShaderObj(COMP_CTX_ID, DRAW_VS, PIPE_SHADER_VERTEX, kCompVS) ||
        !CreateShaderObj(COMP_CTX_ID, DRAW_FS, PIPE_SHADER_FRAGMENT, kCompFS))
    { SerialPuts("virtio_gpu: draw shader create failed\n"); return false; }

    g_drawReady = true;
    return true;
}

// Forward decls for blur setup helpers (defined with the draw self-test helpers).
// One-time setup of the frosted-glass backdrop-blur objects: two fixed-size
// host-only render targets (downsample/ping-pong), their surfaces + sampler
// views, a linear+clamp-to-edge sampler, a full-screen vertex buffer, and the
// separable gaussian shaders. Leaves g_blurReady false (blur disabled, the
// compositor simply never calls BlurBarrier effectively) on any failure.
static bool SetupGpuBlur()
{
    if (!g_drawReady) return false;

    // Two host-only RTs (no guest backing — sampled + rendered on the host).
    if (!ResourceCreate3D(COMP_BLUR_A_RES, VIRGL_FORMAT_B8G8R8X8_UNORM,
                          VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                          COMP_BLUR_W, COMP_BLUR_H) ||
        !CtxAttachResource(COMP_CTX_ID, COMP_BLUR_A_RES) ||
        !ResourceCreate3D(COMP_BLUR_B_RES, VIRGL_FORMAT_B8G8R8X8_UNORM,
                          VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                          COMP_BLUR_W, COMP_BLUR_H) ||
        !CtxAttachResource(COMP_CTX_ID, COMP_BLUR_B_RES))
    { SerialPuts("virtio_gpu: blur RT create failed\n"); return false; }

    // Full-screen vertex buffer (NDC quad, uv 0..1, opacity 1). 4 verts * 5 f32.
    {
        const uint32_t vbytes = 4 * 5 * 4;
        PhysicalAddress vp = PmmAllocPages(AlignUp(vbytes, 4096) / 4096, MemTag::Device, KernelPid);
        if (!vp) { SerialPuts("virtio_gpu: blur vbuf alloc failed\n"); return false; }
        g_blurVtxPhys = vp.raw();
        g_blurVtxBuf  = reinterpret_cast<uint32_t*>(PhysToVirt(vp).raw());
        uint32_t* v = g_blurVtxBuf;
        const uint32_t one = F32Bits(1.0f), zero = F32Bits(0.0f), nOne = F32Bits(-1.0f);
        // (x,y,u,v,op): TL(-1,-1,0,0) TR(1,-1,1,0) BL(-1,1,0,1) BR(1,1,1,1)
        v[0]=nOne; v[1]=nOne; v[2]=zero; v[3]=zero; v[4]=one;
        v[5]=one;  v[6]=nOne; v[7]=one;  v[8]=zero; v[9]=one;
        v[10]=nOne;v[11]=one; v[12]=zero;v[13]=one; v[14]=one;
        v[15]=one; v[16]=one; v[17]=one; v[18]=one; v[19]=one;
        if (!ResourceCreateBuffer(COMP_BLURVTX_RES, VIRGL_BIND_VERTEX_BUFFER, vbytes) ||
            !ResourceAttachBackingContig(COMP_BLURVTX_RES, g_blurVtxPhys, vbytes) ||
            !CtxAttachResource(COMP_CTX_ID, COMP_BLURVTX_RES) ||
            !TransferToHostBuffer(COMP_CTX_ID, COMP_BLURVTX_RES, vbytes))
        { SerialPuts("virtio_gpu: blur vbuf resource failed\n"); return false; }
    }

    // Surfaces (render targets), linear+clamp-to-edge sampler, sampler views.
    {
        auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
        memset(sub, 0, sizeof(*sub));
        sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = COMP_CTX_ID;
        uint32_t* dw = ComposeDwBase(); uint32_t n = 0;
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
        dw[n++] = BLUR_A_SURF; dw[n++] = COMP_BLUR_A_RES; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
        dw[n++] = 0; dw[n++] = 0;
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
        dw[n++] = BLUR_B_SURF; dw[n++] = COMP_BLUR_B_RES; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
        dw[n++] = 0; dw[n++] = 0;
        // Linear, clamp-to-edge sampler: wrap_s/t/r = CLAMP_TO_EDGE(2),
        // min/mag image filter = LINEAR(1).  S0 = 2|(2<<3)|(2<<6)|(1<<9)|(1<<13).
        uint32_t s0lin = 2u | (2u << 3) | (2u << 6) | (1u << 9) | (1u << 13);
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_STATE, 9);
        dw[n++] = DRAW_SAMP_LIN; dw[n++] = s0lin;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        sub->size = n * 4;
        if (!CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP)))
        { SerialPuts("virtio_gpu: blur surfaces/sampler failed\n"); return false; }
    }
    // Sampler views for the two blur RTs (identity swizzle 0x688).
    {
        g_blurAView = g_drawNextSview++;
        g_blurBView = g_drawNextSview++;
        auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
        memset(sub, 0, sizeof(*sub));
        sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = COMP_CTX_ID;
        uint32_t* dw = ComposeDwBase(); uint32_t n = 0;
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 6);
        dw[n++] = g_blurAView; dw[n++] = COMP_BLUR_A_RES; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0x688u;
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 6);
        dw[n++] = g_blurBView; dw[n++] = COMP_BLUR_B_RES; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0x688u;
        sub->size = n * 4;
        if (!CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP)))
        { SerialPuts("virtio_gpu: blur sampler-views failed\n"); return false; }
    }
    // Gaussian shaders.
    if (!CreateShaderObj(COMP_CTX_ID, DRAW_FS_BLURH, PIPE_SHADER_FRAGMENT, kBlurH) ||
        !CreateShaderObj(COMP_CTX_ID, DRAW_FS_BLURV, PIPE_SHADER_FRAGMENT, kBlurV))
    { SerialPuts("virtio_gpu: blur shader create failed\n"); return false; }

    g_blurReady = true;
    return true;
}

// One-time setup of the persistent compositor context + scanout render-target.
// The scanout RT has NO guest backing (host-only); FLUSH presents it directly.
static bool SetupGpuCompositor(uint32_t w, uint32_t h)
{
    if (!(g_gpu3dFeatures & VIRTIO_GPU_F_VIRGL)) return false;

    if (!CtxCreate(COMP_CTX_ID, VIRTIO_GPU_CAPSET_VIRGL))
    { SerialPuts("virtio_gpu: comp CTX_CREATE failed\n"); return false; }
    if (!ResourceCreate3D(COMP_SCANOUT_RES, VIRGL_FORMAT_B8G8R8X8_UNORM,
                          VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SCANOUT |
                          VIRGL_BIND_SAMPLER_VIEW, w, h))
    { SerialPuts("virtio_gpu: comp scanout RT create failed\n"); return false; }
    if (!CtxAttachResource(COMP_CTX_ID, COMP_SCANOUT_RES))
    { SerialPuts("virtio_gpu: comp scanout ctx-attach failed\n"); return false; }

    // Create the scanout surface object ONCE; reused as the clear target each
    // frame (recreating the same handle every frame would error in virgl).
    {
        auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
        memset(sub, 0, sizeof(*sub));
        sub->hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
        sub->hdr.ctx_id = COMP_CTX_ID;
        uint32_t* dw = ComposeDwBase();
        uint32_t n = 0;
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
        dw[n++] = COMP_SCANOUT_SURF;
        dw[n++] = COMP_SCANOUT_RES;
        dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
        dw[n++] = 0;
        dw[n++] = 0;
        sub->size = n * 4;
        if (!CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP)))
        { SerialPuts("virtio_gpu: comp scanout surface create failed\n"); return false; }
    }

    g_compScanoutW = w;
    g_compScanoutH = h;

    // Thumbnail readback RT: a small render-target with a contiguous backing.
    // CaptureThumb BLITs the full scanout into this (downscaled) and reads it
    // back for in-guest visual verification (no host display needed).
    {
        uint32_t tbytes = COMP_THUMB_W * COMP_THUMB_H * 4;
        PhysicalAddress tphys = PmmAllocPages(AlignUp(tbytes, 4096) / 4096,
                                              MemTag::Device, KernelPid);
        if (tphys)
        {
            g_compThumbPhys = tphys.raw();
            g_compThumbBuf  = reinterpret_cast<uint32_t*>(PhysToVirt(tphys).raw());
            memset(g_compThumbBuf, 0, tbytes);
            if (ResourceCreate3D(COMP_THUMB_RES, VIRGL_FORMAT_B8G8R8X8_UNORM,
                                 VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                                 COMP_THUMB_W, COMP_THUMB_H) &&
                ResourceAttachBackingContig(COMP_THUMB_RES, g_compThumbPhys, tbytes))
            {
                CtxAttachResource(COMP_CTX_ID, COMP_THUMB_RES);
            }
            else
            {
                g_compThumbBuf = nullptr;   // thumbnail unavailable; non-fatal
                SerialPuts("virtio_gpu: comp thumbnail RT unavailable\n");
            }
        }
    }

    g_compReady    = true;
    SerialPrintf("virtio_gpu: GPU compositor ready (scanout RT %ux%u)\n", w, h);

    // Set up the DRAW (textured-quad) pipeline objects. If this fails we leave
    // g_drawReady false and DrawSupported() reports false, so the compositor uses
    // the CPU compositor path instead (the GPU path requires DRAW).
    if (SetupGpuDrawPipeline())
        SerialPuts("virtio_gpu: GPU DRAW pipeline ready (textured-quad composition available)\n");
    else
        SerialPuts("virtio_gpu: GPU DRAW pipeline unavailable — CPU compositor will be used\n");

    // Optional frosted-glass backdrop blur (needs the DRAW pipeline). Non-fatal:
    // if it fails the compositor just never gets a usable blurred backdrop.
    if (g_drawReady)
    {
        if (SetupGpuBlur())
            SerialPuts("virtio_gpu: GPU backdrop-blur pipeline ready (frosted glass available)\n");
        else
            SerialPuts("virtio_gpu: GPU backdrop-blur unavailable — chrome will not blur\n");
    }
    return true;
}

static GpuTexId CompCreateTexture(uint32_t w, uint32_t h, uint64_t backingVaddr, bool alpha)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    if (!g_compReady || w == 0 || h == 0) return 0;
    uint32_t slot = MAX_GPU_TEXTURES;
    for (uint32_t i = 0; i < MAX_GPU_TEXTURES; ++i)
        if (!g_gpuTextures[i].used) { slot = i; break; }
    if (slot == MAX_GPU_TEXTURES) { SerialPuts("virtio_gpu: comp texture table full\n"); return 0; }

    uint32_t format = alpha ? VIRGL_FORMAT_B8G8R8A8_UNORM : VIRGL_FORMAT_B8G8R8X8_UNORM;
    uint32_t resId = g_compNextRes++;
    if (!ResourceCreate3D(resId, format, VIRGL_BIND_SAMPLER_VIEW, w, h))
    { SerialPuts("virtio_gpu: comp tex create failed\n"); return 0; }
    if (!ResourceAttachBackingVirt(resId, backingVaddr, w * h * 4))
    { SerialPuts("virtio_gpu: comp tex backing failed\n"); return 0; }
    if (!CtxAttachResource(COMP_CTX_ID, resId))
    { SerialPuts("virtio_gpu: comp tex ctx-attach failed\n"); return 0; }

    g_gpuTextures[slot] = { resId, w, h, format, true, 0 };
    GpuTexture& gt = g_gpuTextures[slot];

    // In DRAW mode, every texture needs a sampler view to be drawn as a quad.
    // Create it once here (outside any frame-compose accumulation, so it can
    // safely issue its own submit). Identity swizzle (0x688): R->R,G->G,B->B,A->A.
    if (g_drawReady)
    {
        uint32_t sv = g_drawNextSview++;
        auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
        memset(sub, 0, sizeof(*sub));
        sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = COMP_CTX_ID;
        uint32_t* dw = ComposeDwBase(); uint32_t n = 0;
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 6);
        dw[n++] = sv; dw[n++] = resId; dw[n++] = format;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0x688u;
        sub->size = n * 4;
        if (CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP)))
            gt.sview = sv;
        else
            SerialPuts("virtio_gpu: comp tex sampler-view create failed\n");
    }
    return slot + 1;   // GpuTexId is 1-based
}

static GpuTexture* CompTex(GpuTexId t)
{
    if (t == 0 || t > MAX_GPU_TEXTURES) return nullptr;
    GpuTexture* gt = &g_gpuTextures[t - 1];
    return gt->used ? gt : nullptr;
}

static void CompDestroyTexture(GpuTexId t)
{
    GpuTexture* gt = CompTex(t);
    if (!gt) return;
    // Detach via attach-backing with zero entries would be ideal; for now just
    // free the table slot (resource id is not recycled — monotonic). The host
    // resource is reclaimed at context teardown / device reset.
    gt->used = false;
}

static void CompUpdateTexture(GpuTexId t, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    GpuTexture* gt = CompTex(t);
    if (!gt) return;
    if (x >= gt->w || y >= gt->h) return;
    if (x + w > gt->w) w = gt->w - x;
    if (y + h > gt->h) h = gt->h - y;
    if (w == 0 || h == 0) return;
    TransferToHost3D(COMP_CTX_ID, gt->resId, x, y, w, h, gt->w, gt->h);
}

static void CompBeginFrame(uint32_t clearArgb)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    if (!g_compReady) return;
    (void)clearArgb;   // see note below
    // First GPU frame: point scanout 0 at the RT (switches away from the 2D FB).
    if (!g_compScanoutBound)
    {
        if (SetScanout(0, COMP_SCANOUT_RES, g_compScanoutW, g_compScanoutH))
            g_compScanoutBound = true;
    }

    // Reset the DRAW record list (whether or not DRAW is used this frame). If
    // the compositor records quads via CompDrawQuad, CompEndFrame builds the
    // batched draw stream and ignores the BLIT compose stream below.
    g_drawQuadCount = 0;
    g_blurBarrierCount = 0;        // no backdrop barriers unless BlurBarrier() is called

    // Start a fresh compose stream: SET_FRAMEBUFFER_STATE(scanout surf) + CLEAR.
    // Clear to opaque black. (The kernel builds with -mno-sse / soft-float is not
    // linked, so we use only the literal float constants 0.0f/1.0f, which the
    // compiler folds to immediates — no runtime float ops. The wallpaper layer,
    // blitted first each frame, provides the actual desktop background; black is
    // only visible in any region the wallpaper does not cover.)
    uint32_t* dw = ComposeDwBase();
    uint32_t n = 0;
    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    dw[n++] = 1;                   // nr_cbufs
    dw[n++] = 0;                   // zsurf
    dw[n++] = COMP_SCANOUT_SURF;   // cbuf[0]

    dw[n++] = VirglCmd0(VIRGL_CCMD_CLEAR, 0, 8);
    dw[n++] = PIPE_CLEAR_COLOR0;
    dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(0.0f);
    dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(1.0f);
    dw[n++] = 0; dw[n++] = 0;      // depth
    dw[n++] = 0;                   // stencil

    g_composeN = n;
}

// Record a textured-quad layer for the DRAW path: sample (sx,sy,sw,sh) of the
// source texture into screen rect (dx,dy,dw,dh), with optional src-alpha blend.
// uv is precomputed here (needs the texture dims); vertices + GPU commands are
// emitted in CompEndFrame. `opacity` is accepted for interface parity but not
// yet applied (opaque-parity milestone; per-window opacity is a follow-up).
static void CompDrawQuad(GpuTexId src,
                         uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                         uint32_t dx, uint32_t dy, uint32_t dw_, uint32_t dh,
                         uint32_t opacity, bool alphaBlend)
{
    if (!g_compReady || !g_drawReady || g_drawQuadCount >= MAX_DRAW_QUADS) return;

    // Special source: the blurred backdrop. Sample it at the destination rect's
    // screen-space uv (normalized to the scanout), so the quad shows the frosted
    // scene behind it. No texture/sview lookup; resolved to g_blurAView at emit.
    if (src == brook::GPU_TEX_BLUR_BACKDROP)
    {
        if (!g_blurReady || g_compScanoutW == 0 || g_compScanoutH == 0) return;
        DrawQuadRec& q = g_drawQuads[g_drawQuadCount++];
        q.dx  = static_cast<int32_t>(dx);
        q.dy  = static_cast<int32_t>(dy);
        q.dx2 = static_cast<int32_t>(dx + dw_);
        q.dy2 = static_cast<int32_t>(dy + dh);
        q.u0 = FracBits(static_cast<int32_t>(dx),       static_cast<int32_t>(g_compScanoutW));
        q.v0 = FracBits(static_cast<int32_t>(dy),       static_cast<int32_t>(g_compScanoutH));
        q.u1 = FracBits(static_cast<int32_t>(dx + dw_), static_cast<int32_t>(g_compScanoutW));
        q.v1 = FracBits(static_cast<int32_t>(dy + dh),  static_cast<int32_t>(g_compScanoutH));
        q.opacity = F32Bits(1.0f);
        q.sview = g_blurAView;          // final blurred RT
        q.blend = DRAW_BLEND;           // opaque (the glass chrome blends on top)
        q.flags = QUAD_SAMPLE_BLUR;
        return;
    }

    GpuTexture* gt = CompTex(src);
    if (!gt || gt->sview == 0) return;

    DrawQuadRec& q = g_drawQuads[g_drawQuadCount++];
    q.dx  = static_cast<int32_t>(dx);
    q.dy  = static_cast<int32_t>(dy);
    q.dx2 = static_cast<int32_t>(dx + dw_);
    q.dy2 = static_cast<int32_t>(dy + dh);
    q.u0 = FracBits(static_cast<int32_t>(sx),       static_cast<int32_t>(gt->w));
    q.v0 = FracBits(static_cast<int32_t>(sy),       static_cast<int32_t>(gt->h));
    q.u1 = FracBits(static_cast<int32_t>(sx + sw),  static_cast<int32_t>(gt->w));
    q.v1 = FracBits(static_cast<int32_t>(sy + sh),  static_cast<int32_t>(gt->h));
    // Opacity in [0,1] as float bits; 255 -> 1.0 (sampled alpha passes through
    // unchanged, preserving opaque parity). A translucent window forces the
    // src-alpha-over blend so the scaled alpha actually composites.
    q.opacity = (opacity >= 255) ? F32Bits(1.0f)
                                 : FracBits(static_cast<int32_t>(opacity), 255);
    q.sview = gt->sview;
    q.blend = (alphaBlend || opacity < 255) ? DRAW_BLEND_ON : DRAW_BLEND;
    q.flags = 0;
}

// Record the backdrop barrier: subsequent GPU_TEX_BLUR_BACKDROP quads sample the
// blur of everything recorded before this point. Records the split index and the
// app-requested blur strength (scales the number of gaussian iterations).
static void CompBlurBarrier(uint32_t strength)
{
    if (!g_blurReady) return;
    if (g_blurBarrierCount >= MAX_BLUR_BARRIERS) return;   // cap cost; extra glass windows skip frosting
    g_blurBarrierIdx[g_blurBarrierCount]      = g_drawQuadCount;
    g_blurBarrierStrength[g_blurBarrierCount] = strength;
    g_blurBarrierCount++;
}


// Build the per-frame vertex buffer from the recorded quads, upload it, then
// emit one batched compose stream (framebuffer + clear + state binds, then per
// quad: blend, sampler view, vertex-buffer offset, draw) and present. Keeps the
// vertex upload and the draw submit as separate sequential submits so the
// command buffer is never aliased.
static void CompEndFrameDraw()
{
    // 1. Fill the vertex staging buffer: 4 verts/quad, {pos.xy, uv.xy, opacity}.
    //    NDC positions derived from integer pixel rects via NdcBits (no FP).
    int32_t W = static_cast<int32_t>(g_compScanoutW);
    int32_t H = static_cast<int32_t>(g_compScanoutH);
    for (uint32_t i = 0; i < g_drawQuadCount; ++i)
    {
        const DrawQuadRec& q = g_drawQuads[i];
        uint32_t nx0 = NdcBits(q.dx,  W), ny0 = NdcBits(q.dy,  H);
        uint32_t nx1 = NdcBits(q.dx2, W), ny1 = NdcBits(q.dy2, H);
        uint32_t a = q.opacity;
        uint32_t* v = g_drawVtxBuf + i * 20;   // 4 verts * 5 floats
        v[0]=nx0;  v[1]=ny0;  v[2]=q.u0;  v[3]=q.v0;  v[4]=a;
        v[5]=nx1;  v[6]=ny0;  v[7]=q.u1;  v[8]=q.v0;  v[9]=a;
        v[10]=nx0; v[11]=ny1; v[12]=q.u0; v[13]=q.v1; v[14]=a;
        v[15]=nx1; v[16]=ny1; v[17]=q.u1; v[18]=q.v1; v[19]=a;
    }
    if (g_drawQuadCount > 0)
        TransferToHostBuffer(COMP_CTX_ID, COMP_DRAW_VBUF_RES, g_drawQuadCount * 80);

    // 2. Build the compose stream.
    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
    memset(sub, 0, sizeof(*sub));
    sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = COMP_CTX_ID;
    uint32_t* dw = ComposeDwBase(); uint32_t n = 0;

    // Framebuffer + clear (opaque black; wallpaper quad covers it).
    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    dw[n++] = 1; dw[n++] = 0; dw[n++] = COMP_SCANOUT_SURF;
    dw[n++] = VirglCmd0(VIRGL_CCMD_CLEAR, 0, 8);
    dw[n++] = PIPE_CLEAR_COLOR0;
    dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(1.0f);
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
    // Viewport: clip [-1,1] -> [0,W]x[0,H]. scale=(W/2,H/2), translate=(W/2,H/2),
    // built from integer dims via IntToF32Bits (no runtime FP).
    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
    dw[n++] = 0;
    dw[n++] = IntToF32Bits(W / 2); dw[n++] = IntToF32Bits(H / 2); dw[n++] = F32Bits(1.0f);
    dw[n++] = IntToF32Bits(W / 2); dw[n++] = IntToF32Bits(H / 2); dw[n++] = F32Bits(0.0f);
    // State bound once for the whole frame.
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_RASTERIZER, 1);      dw[n++] = DRAW_RAST;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_DSA, 1);             dw[n++] = DRAW_DSA;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 1); dw[n++] = DRAW_VE;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_VS; dw[n++] = PIPE_SHADER_VERTEX;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_FS; dw[n++] = PIPE_SHADER_FRAGMENT;
    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 3);
    dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = DRAW_SAMP;

    uint32_t lastBlend = 0;
    uint32_t lastSamp  = DRAW_SAMP;   // nearest bound above
    uint32_t barrierCursor = 0;       // next barrier to process (indices ascending)
    for (uint32_t i = 0; i < g_drawQuadCount; ++i)
    {
        // At each backdrop barrier landing on this quad, snapshot the scanout
        // (everything drawn so far), downsample it into BLUR_A, run the separable
        // gaussian (H: A->B, V: B->A), then restore the scanout framebuffer/
        // viewport/shader/sampler so the following backdrop quad (which samples
        // BLUR_A via GPU_TEX_BLUR_BACKDROP) composites normally. Multiple barriers
        // per frame (one per glass window, back-to-front) each re-snapshot the
        // current scene, so a glass window frosts exactly what is beneath it —
        // including lower glass windows (BRO-185). One stream, in order — the host
        // serializes it.
        while (barrierCursor < g_blurBarrierCount &&
               g_blurBarrierIdx[barrierCursor] == i && g_blurReady)
        {
            uint32_t barrierStrength = g_blurBarrierStrength[barrierCursor];
            ++barrierCursor;
            // Iterations scale with the app-requested blur strength (a window's
            // blurRadius): each extra H/V gaussian pair widens the effective blur.
            uint32_t iters = (barrierStrength + 3) / 4;   // radius 4->1, 8->2, ...
            if (iters < 1) iters = 1;
            if (iters > 4) iters = 4;                     // cap cost
            if (n + 40 + iters * 30 > (CMD_PAGES - 1) * 1024) { /* no room */ }
            else
            {
                const int32_t bw = static_cast<int32_t>(COMP_BLUR_W);
                const int32_t bh = static_cast<int32_t>(COMP_BLUR_H);
                // Downsample BLIT scanout -> BLUR_A (linear).
                uint32_t s0 = (VIRGL_BLIT_MASK_RGBA & 0xFF) | ((VIRGL_TEX_FILTER_LINEAR & 0x3) << 8);
                dw[n++] = VirglCmd0(VIRGL_CCMD_BLIT, 0, 21);
                dw[n++] = s0; dw[n++] = 0; dw[n++] = 0;
                dw[n++] = COMP_BLUR_A_RES; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
                dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
                dw[n++] = COMP_BLUR_W; dw[n++] = COMP_BLUR_H; dw[n++] = 1;
                dw[n++] = COMP_SCANOUT_RES; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
                dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
                dw[n++] = static_cast<uint32_t>(W); dw[n++] = static_cast<uint32_t>(H); dw[n++] = 1;
                // Opaque blend + linear sampler + blur vertex buffer + blur viewport.
                dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1); dw[n++] = DRAW_BLEND;
                dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 3);
                dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = DRAW_SAMP_LIN;
                dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
                dw[n++] = 0;
                dw[n++] = IntToF32Bits(bw / 2); dw[n++] = IntToF32Bits(bh / 2); dw[n++] = F32Bits(1.0f);
                dw[n++] = IntToF32Bits(bw / 2); dw[n++] = IntToF32Bits(bh / 2); dw[n++] = F32Bits(0.0f);
                dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
                dw[n++] = 20; dw[n++] = 0; dw[n++] = COMP_BLURVTX_RES;
                // `iters` separable passes; each pair leaves the result in BLUR_A.
                for (uint32_t it = 0; it < iters; ++it)
                {
                    // H pass: BLUR_A -> BLUR_B.
                    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
                    dw[n++] = 1; dw[n++] = 0; dw[n++] = BLUR_B_SURF;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_FS_BLURH; dw[n++] = PIPE_SHADER_FRAGMENT;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 3);
                    dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = g_blurAView;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_DRAW_VBO, 0, 12);
                    dw[n++] = 0; dw[n++] = 4; dw[n++] = PIPE_PRIM_TRIANGLE_STRIP; dw[n++] = 0;
                    dw[n++] = 1; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
                    dw[n++] = 0; dw[n++] = 0; dw[n++] = 3; dw[n++] = 0;
                    // V pass: BLUR_B -> BLUR_A.
                    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
                    dw[n++] = 1; dw[n++] = 0; dw[n++] = BLUR_A_SURF;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_FS_BLURV; dw[n++] = PIPE_SHADER_FRAGMENT;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 3);
                    dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = g_blurBView;
                    dw[n++] = VirglCmd0(VIRGL_CCMD_DRAW_VBO, 0, 12);
                    dw[n++] = 0; dw[n++] = 4; dw[n++] = PIPE_PRIM_TRIANGLE_STRIP; dw[n++] = 0;
                    dw[n++] = 1; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
                    dw[n++] = 0; dw[n++] = 0; dw[n++] = 3; dw[n++] = 0;
                }
                // Restore scanout framebuffer + viewport + chrome shader + nearest.
                dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
                dw[n++] = 1; dw[n++] = 0; dw[n++] = COMP_SCANOUT_SURF;
                dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
                dw[n++] = 0;
                dw[n++] = IntToF32Bits(W / 2); dw[n++] = IntToF32Bits(H / 2); dw[n++] = F32Bits(1.0f);
                dw[n++] = IntToF32Bits(W / 2); dw[n++] = IntToF32Bits(H / 2); dw[n++] = F32Bits(0.0f);
                dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = DRAW_FS; dw[n++] = PIPE_SHADER_FRAGMENT;
                dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 3);
                dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = DRAW_SAMP;
                lastBlend = DRAW_BLEND;   // blur bound opaque
                lastSamp  = DRAW_SAMP;
            }
        }

        const DrawQuadRec& q = g_drawQuads[i];
        // ~16 dwords/quad; bail if we'd overflow the compose region (last page
        // is the response buffer).
        if (n + 18 > (CMD_PAGES - 1) * 1024) break;
        if (q.blend != lastBlend) {
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1);
            dw[n++] = q.blend;
            lastBlend = q.blend;
        }
        // Frosted-glass backdrop quads sample the (upscaled) blur RT with linear
        // filtering; everything else stays nearest (crisp 1:1 textures/masks).
        uint32_t wantSamp = (q.flags & QUAD_SAMPLE_BLUR) ? DRAW_SAMP_LIN : DRAW_SAMP;
        if (wantSamp != lastSamp) {
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 3);
            dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = wantSamp;
            lastSamp = wantSamp;
        }
        dw[n++] = VirglCmd0(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 3);
        dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = q.sview;
        dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
        dw[n++] = 20; dw[n++] = i * 80; dw[n++] = COMP_DRAW_VBUF_RES;
        dw[n++] = VirglCmd0(VIRGL_CCMD_DRAW_VBO, 0, 12);
        dw[n++] = 0; dw[n++] = 4; dw[n++] = PIPE_PRIM_TRIANGLE_STRIP; dw[n++] = 0;
        dw[n++] = 1; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 3; dw[n++] = 0;
    }
    sub->size = n * 4;
    SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP);
    g_drawQuadCount = 0;
    g_blurBarrierCount = 0;
    ResourceFlush(COMP_SCANOUT_RES, 0, 0, g_compScanoutW, g_compScanoutH);
}

static void CompEndFrame()
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    if (!g_compReady) return;
    if (g_drawReady && g_drawQuadCount > 0) { CompEndFrameDraw(); return; }
    if (g_composeN == 0) return;
    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
    memset(sub, 0, sizeof(*sub));
    sub->hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
    sub->hdr.ctx_id = COMP_CTX_ID;
    sub->size       = g_composeN * 4;
    SubmitCommand(sizeof(VirtioGpuCmdSubmit) + g_composeN * 4, CMD_RESP_CAP);
    g_composeN = 0;
    // Present the composed scanout RT.
    ResourceFlush(COMP_SCANOUT_RES, 0, 0, g_compScanoutW, g_compScanoutH);
}

// True if the DRAW composition pipeline is set up and ready.
static bool CompDrawSupported()
{
    return g_drawReady;
}

static void CompGetSize(uint32_t* w, uint32_t* h)
{
    if (w) *w = g_compScanoutW;
    if (h) *h = g_compScanoutH;
}

// BLIT the full presented scanout into the small thumbnail RT (downscaled),
// read it back, and copy it out. Returns the pixel count (0 on failure).
static uint32_t CompCaptureThumb(uint32_t* out, uint32_t maxPixels,
                                 uint32_t* outW, uint32_t* outH)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    if (!g_compReady || !g_compThumbBuf || !out) return 0;

    // One-BLIT submit: scanout (full) -> thumbnail (scaled down).
    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
    memset(sub, 0, sizeof(*sub));
    sub->hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
    sub->hdr.ctx_id = COMP_CTX_ID;
    uint32_t* dw = ComposeDwBase();
    uint32_t n = 0;
    uint32_t s0 = (VIRGL_BLIT_MASK_RGBA & 0xFF) | ((VIRGL_TEX_FILTER_NEAREST & 0x3) << 8);
    dw[n++] = VirglCmd0(VIRGL_CCMD_BLIT, 0, 21);
    dw[n++] = s0; dw[n++] = 0; dw[n++] = 0;
    dw[n++] = COMP_THUMB_RES; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
    dw[n++] = COMP_THUMB_W; dw[n++] = COMP_THUMB_H; dw[n++] = 1;
    dw[n++] = COMP_SCANOUT_RES; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
    dw[n++] = g_compScanoutW; dw[n++] = g_compScanoutH; dw[n++] = 1;
    sub->size = n * 4;
    if (!CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP)))
        return 0;

    if (!TransferFromHost3D(COMP_CTX_ID, COMP_THUMB_RES, COMP_THUMB_W, COMP_THUMB_H))
        return 0;

    uint32_t count = COMP_THUMB_W * COMP_THUMB_H;
    if (count > maxPixels) count = maxPixels;
    for (uint32_t i = 0; i < count; ++i) out[i] = g_compThumbBuf[i];
    if (outW) *outW = COMP_THUMB_W;
    if (outH) *outH = COMP_THUMB_H;
    return count;
}

// Capture the full presented scanout at native resolution. Lazily creates a
// full-size readback RT (contiguous backing, ~8MB at 1080p), BLITs the scanout
// 1:1 into it, reads it back, and returns a pointer to the readback buffer.
static const uint32_t* CompCaptureFull(uint32_t* outW, uint32_t* outH)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    if (!g_compReady || g_compScanoutW == 0) return nullptr;

    if (!g_compFullReady)
    {
        uint32_t fbytes = g_compScanoutW * g_compScanoutH * 4;
        PhysicalAddress fphys = PmmAllocPages(AlignUp(fbytes, 4096) / 4096,
                                              MemTag::Device, KernelPid);
        if (!fphys) { SerialPuts("virtio_gpu: comp full-res RT alloc failed\n"); return nullptr; }
        g_compFullBuf = reinterpret_cast<uint32_t*>(PhysToVirt(fphys).raw());
        memset(g_compFullBuf, 0, fbytes);
        if (!ResourceCreate3D(COMP_FULL_RES, VIRGL_FORMAT_B8G8R8X8_UNORM,
                              VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                              g_compScanoutW, g_compScanoutH) ||
            !ResourceAttachBackingContig(COMP_FULL_RES, fphys.raw(), fbytes))
        { SerialPuts("virtio_gpu: comp full-res RT create failed\n"); g_compFullBuf = nullptr; return nullptr; }
        CtxAttachResource(COMP_CTX_ID, COMP_FULL_RES);
        g_compFullReady = true;
    }
    if (!g_compFullBuf) return nullptr;

    // BLIT scanout -> full-res RT 1:1.
    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
    memset(sub, 0, sizeof(*sub));
    sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = COMP_CTX_ID;
    uint32_t* dw = ComposeDwBase(); uint32_t n = 0;
    uint32_t s0 = (VIRGL_BLIT_MASK_RGBA & 0xFF) | ((VIRGL_TEX_FILTER_NEAREST & 0x3) << 8);
    dw[n++] = VirglCmd0(VIRGL_CCMD_BLIT, 0, 21);
    dw[n++] = s0; dw[n++] = 0; dw[n++] = 0;
    dw[n++] = COMP_FULL_RES; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
    dw[n++] = g_compScanoutW; dw[n++] = g_compScanoutH; dw[n++] = 1;
    dw[n++] = COMP_SCANOUT_RES; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
    dw[n++] = g_compScanoutW; dw[n++] = g_compScanoutH; dw[n++] = 1;
    sub->size = n * 4;
    if (!CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP)))
        return nullptr;
    if (!TransferFromHost3D(COMP_CTX_ID, COMP_FULL_RES, g_compScanoutW, g_compScanoutH))
        return nullptr;
    if (outW) *outW = g_compScanoutW;
    if (outH) *outH = g_compScanoutH;
    return g_compFullBuf;
}

// ===========================================================================
// App-GPU path: per-process virgl contexts for userspace GL-style rendering.
// Each app gets its own virgl context + a private resource-id space, kept apart
// from the compositor's COMP_CTX_ID / COMP_* resources. The app submits virgl
// command-stream dwords; we relay them via SUBMIT_3D, exactly as the compositor
// does for its own stream. Resource ids are namespaced per context so apps can
// use small local ids without colliding with each other or the kernel.
// ===========================================================================
static constexpr uint32_t APP_FIRST_CTX  = 0x100;   // app ctx ids start here (1,2 = kernel)
static constexpr uint32_t APP_RES_STRIDE = 0x1000;  // per-ctx resource-id block
static constexpr uint32_t APP_RES_BASE   = 0x10000; // app resource ids start here
static constexpr uint32_t MAX_APP_CTX    = 16;
struct AppCtx { bool used; uint32_t ctxId; uint32_t pid; };
static AppCtx g_appCtx[MAX_APP_CTX] = {};

// Kernel bounce-buffers backing app resources uploaded from user memory (e.g.
// vertex buffers). Tracked so they're freed when the owning context is gone.
struct AppUpload { bool used; int32_t ctxId; uint64_t vaddr; uint64_t pages; };
static constexpr uint32_t MAX_APP_UPLOADS = 64;
static AppUpload g_appUploads[MAX_APP_UPLOADS] = {};

// Map an app's (ctxId, local resId) to a globally-unique host resource id, so an
// app can pass small local resource ids (1,2,3,...) without collisions.
static inline uint32_t AppResGlobal(uint32_t ctxId, uint32_t localRes)
{
    return APP_RES_BASE + (ctxId - APP_FIRST_CTX) * APP_RES_STRIDE + (localRes & (APP_RES_STRIDE - 1));
}

// Validate that `gres` is a global resource id inside `ctxId`'s private block.
// App resource ids are opaque host-global handles: the app receives them from
// ResourceCreate3D and uses the SAME id both in syscalls AND inside the virgl
// command streams it submits (where resource ids must be host-global). Bounding
// each context to its own APP_RES_STRIDE block keeps apps isolated from each
// other and from the compositor/self-test resources.
static inline bool AppResValid(int32_t ctxId, uint32_t gres)
{
    if (ctxId < (int32_t)APP_FIRST_CTX) return false;
    uint32_t base = APP_RES_BASE + ((uint32_t)ctxId - APP_FIRST_CTX) * APP_RES_STRIDE;
    return gres > base && gres < base + APP_RES_STRIDE;
}

static int32_t AppCtxCreate(uint32_t pid)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    if (!g_compReady) return -1;
    for (uint32_t i = 0; i < MAX_APP_CTX; ++i)
    {
        if (g_appCtx[i].used) continue;
        uint32_t ctxId = APP_FIRST_CTX + i;
        if (!CtxCreate(ctxId, VIRTIO_GPU_CAPSET_VIRGL))
        { SerialPuts("virtio_gpu: app ctx create failed\n"); return -1; }
        g_appCtx[i] = { true, ctxId, pid };
        SerialPrintf("virtio_gpu: app virgl ctx %u for pid %u\n", ctxId, pid);
        return static_cast<int32_t>(ctxId);
    }
    SerialPuts("virtio_gpu: app ctx table full\n");
    return -1;
}

static AppCtx* AppCtxFind(int32_t ctxId)
{
    if (ctxId < (int32_t)APP_FIRST_CTX) return nullptr;
    uint32_t i = (uint32_t)ctxId - APP_FIRST_CTX;
    if (i >= MAX_APP_CTX || !g_appCtx[i].used) return nullptr;
    return &g_appCtx[i];
}

static void AppCtxDestroy(int32_t ctxId)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c) return;
    auto* req = reinterpret_cast<VirtioGpuCtrlHdr*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->type   = VIRTIO_GPU_CMD_CTX_DESTROY;
    req->ctx_id = (uint32_t)ctxId;
    SubmitCommand(sizeof(*req), CMD_RESP_CAP);
    c->used = false;
    // Free any upload bounce-buffers owned by this context.
    for (uint32_t i = 0; i < MAX_APP_UPLOADS; ++i)
        if (g_appUploads[i].used && g_appUploads[i].ctxId == ctxId)
        {
            VmmFreePages(VirtualAddress(g_appUploads[i].vaddr), g_appUploads[i].pages);
            g_appUploads[i].used = false;
        }
    SerialPrintf("virtio_gpu: app virgl ctx %d destroyed\n", ctxId);
}

static int32_t AppResourceCreate3D(int32_t ctxId, uint32_t format, uint32_t bind,
                                   uint32_t w, uint32_t h)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c || w == 0 || h == 0) return -1;
    // Resources are named to the app by their host-global id (1-based per ctx,
    // mapped into this context's private block) so the app can reference the
    // same id inside the virgl streams it submits, where ids are host-global.
    static uint32_t s_localCounter[MAX_APP_CTX] = {};
    uint32_t i = (uint32_t)ctxId - APP_FIRST_CTX;
    uint32_t localRes = ++s_localCounter[i];
    if (localRes >= APP_RES_STRIDE) return -1;
    uint32_t gres = AppResGlobal((uint32_t)ctxId, localRes);
    if (!ResourceCreate3D(gres, format, bind, w, h)) return -1;
    if (!CtxAttachResource((uint32_t)ctxId, gres)) return -1;
    return static_cast<int32_t>(gres);
}

static int32_t AppResourceAttachUser(int32_t ctxId, int32_t gres,
                                     uint64_t vaddr, uint32_t bytes)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c || !AppResValid(ctxId, (uint32_t)gres) || bytes == 0) return -1;
    return ResourceAttachBackingVirt((uint32_t)gres, vaddr, bytes) ? 0 : -1;
}

// Create a vertex-buffer resource for `ctxId`, back it with a kernel bounce
// buffer holding a copy of `bytes` from `src` (kernel-readable; the syscall
// validated it), attach it to the context, and push the contents to the host.
// Returns the new resource's host-global id (usable directly in the app's virgl
// SET_VERTEX_BUFFERS command) or <0. The bounce buffer is freed on ctx destroy.
static int32_t AppBufferUpload(int32_t ctxId, const void* src, uint32_t bytes)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c || !src || bytes == 0 || bytes > 16u * 1024 * 1024) return -1;
    uint32_t slot = MAX_APP_UPLOADS;
    for (uint32_t i = 0; i < MAX_APP_UPLOADS; ++i)
        if (!g_appUploads[i].used) { slot = i; break; }
    if (slot == MAX_APP_UPLOADS) return -1;

    static uint32_t s_bufCounter[MAX_APP_CTX] = {};
    uint32_t ci = (uint32_t)ctxId - APP_FIRST_CTX;
    // Buffer ids share the per-ctx block but grow downward from the top so they
    // never collide with ResourceCreate3D's upward-growing texture ids.
    uint32_t localRes = (APP_RES_STRIDE - 1) - (++s_bufCounter[ci]);
    if (s_bufCounter[ci] >= APP_RES_STRIDE / 2) return -1;
    uint32_t gres = AppResGlobal((uint32_t)ctxId, localRes);

    uint64_t pages = (bytes + 4095) / 4096;
    VirtualAddress va = VmmAllocPages(pages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!va) return -1;
    memcpy(reinterpret_cast<void*>(va.raw()), src, bytes);
    if (!ResourceCreateBuffer(gres, VIRGL_BIND_VERTEX_BUFFER, bytes) ||
        !ResourceAttachBackingVirt(gres, va.raw(), bytes) ||
        !CtxAttachResource((uint32_t)ctxId, gres) ||
        !TransferToHostBuffer((uint32_t)ctxId, gres, bytes))
    { VmmFreePages(va, pages); return -1; }
    g_appUploads[slot] = { true, ctxId, va.raw(), pages };
    return static_cast<int32_t>(gres);
}

static int32_t AppSubmit3D(int32_t ctxId, const uint32_t* dwords, uint32_t n)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c || !dwords || n == 0) return -1;
    // Bound the stream to the request region (last page is the response buffer).
    const uint32_t maxDw = ((CMD_PAGES - 1) * 4096 - sizeof(VirtioGpuCmdSubmit)) / 4;
    if (n > maxDw) return -1;
    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
    memset(sub, 0, sizeof(*sub));
    sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = (uint32_t)ctxId;
    uint32_t* dst = reinterpret_cast<uint32_t*>(g_cmdBuf + CMD_REQ_OFF + sizeof(VirtioGpuCmdSubmit));
    for (uint32_t i = 0; i < n; ++i) dst[i] = dwords[i];
    sub->size = n * 4;
    return CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP)) ? 0 : -1;
}

static int32_t AppTransfer3D(int32_t ctxId, int32_t gres, int dir,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t texW, uint32_t texH)
{
    GpuCmdGuard _gcg;  // BRO-189: serialize control-queue submits
    AppCtx* c = AppCtxFind(ctxId);
    if (!c || !AppResValid(ctxId, (uint32_t)gres) || w == 0 || h == 0) return -1;
    if (dir == 0)
        return TransferToHost3D((uint32_t)ctxId, (uint32_t)gres, x, y, w, h, texW, texH) ? 0 : -1;
    return TransferFromHost3D((uint32_t)ctxId, (uint32_t)gres, w, h) ? 0 : -1;
}

static const brook::GpuAppOps g_gpuAppOps = {
    "virtio-gpu-app",
    AppCtxCreate,
    AppCtxDestroy,
    AppResourceCreate3D,
    AppResourceAttachUser,
    AppBufferUpload,
    AppSubmit3D,
    AppTransfer3D,
    GetCapsetBlob,
};

static const brook::GpuCompositorOps g_gpuCompositorOps = {
    "virtio-gpu-draw",
    CompCreateTexture,
    CompDestroyTexture,
    CompUpdateTexture,
    CompBeginFrame,
    CompEndFrame,
    CompGetSize,
    CompCaptureThumb,
    CompCaptureFull,
    CompDrawSupported,
    CompDrawQuad,
    CompBlurBarrier,
};

// Readback self-test of the real compositor ops: build a scattered-backed
// texture (VmmAllocPages → physically non-contiguous), fill it green via its
// virtual mapping, upload it, then compose (clear blue + blit) into a small
// readback-backed scanout RT and verify. Exercises the exact CreateTexture /
// UpdateTexture / blit path the window compositor uses. Uses a temporary 256x256
// scanout with a backing for verification, separate from the real (host-only)
// scanout RT.
static void VirtioGpuCompositorSelfTest()
{
    if (!(g_gpu3dFeatures & VIRTIO_GPU_F_VIRGL)) return;

    const uint32_t tw = 96, th = 96;
    const uint32_t tbytes = tw * th * 4;
    const uint32_t tpages = AlignUp(tbytes, 4096) / 4096;
    // Scattered source buffer (kernel virtual, physically non-contiguous).
    VirtualAddress src = VmmAllocPages(tpages, VMM_WRITABLE, MemTag::Device, KernelPid);
    if (!src) { SerialPuts("virtio_gpu: comp self-test src alloc failed\n"); return; }
    uint32_t* srcPix = reinterpret_cast<uint32_t*>(src.raw());
    for (uint32_t i = 0; i < tw * th; ++i) srcPix[i] = 0xFF00FF00u; // opaque green

    // Temporary readback scanout RT (256x256, contiguous backing for readback).
    const uint32_t sdim = 256, sbytes = sdim * sdim * 4;
    PhysicalAddress sphys = PmmAllocPages(AlignUp(sbytes, 4096) / 4096, MemTag::Device, KernelPid);
    if (!sphys) { SerialPuts("virtio_gpu: comp self-test scanout alloc failed\n"); return; }
    uint32_t* sread = reinterpret_cast<uint32_t*>(PhysToVirt(sphys).raw());
    memset(sread, 0, sbytes);

    const uint32_t TST_CTX = 3, TST_RT = 64, TST_SURF = 9, TST_TEX = 65;
    if (!CtxCreate(TST_CTX, VIRTIO_GPU_CAPSET_VIRGL)) { SerialPuts("comp-test ctx fail\n"); return; }
    if (!ResourceCreate3D(TST_RT, VIRGL_FORMAT_B8G8R8X8_UNORM,
                          VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW, sdim, sdim))
    { SerialPuts("comp-test RT fail\n"); return; }
    if (!ResourceAttachBackingContig(TST_RT, sphys.raw(), sbytes)) { SerialPuts("comp-test RT backing fail\n"); return; }
    if (!CtxAttachResource(TST_CTX, TST_RT)) { SerialPuts("comp-test RT attach fail\n"); return; }

    // Scattered-backed texture from the VmmAllocPages buffer.
    if (!ResourceCreate3D(TST_TEX, VIRGL_FORMAT_B8G8R8X8_UNORM, VIRGL_BIND_SAMPLER_VIEW, tw, th))
    { SerialPuts("comp-test tex fail\n"); return; }
    if (!ResourceAttachBackingVirt(TST_TEX, src.raw(), tbytes))
    { SerialPuts("comp-test tex scattered backing fail\n"); return; }
    if (!CtxAttachResource(TST_CTX, TST_TEX)) { SerialPuts("comp-test tex attach fail\n"); return; }
    if (!TransferToHost3D(TST_CTX, TST_TEX, 0, 0, tw, th, tw, th))
    { SerialPuts("comp-test tex upload fail\n"); return; }

    // Compose: clear blue, blit the green texture to (32,32)-(128,128).
    {
        auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
        memset(sub, 0, sizeof(*sub));
        sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = TST_CTX;
        uint32_t* dw = ComposeDwBase(); uint32_t n = 0;
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
        dw[n++] = TST_SURF; dw[n++] = TST_RT; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM; dw[n++] = 0; dw[n++] = 0;
        dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
        dw[n++] = 1; dw[n++] = 0; dw[n++] = TST_SURF;
        dw[n++] = VirglCmd0(VIRGL_CCMD_CLEAR, 0, 8);
        dw[n++] = PIPE_CLEAR_COLOR0;
        dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(0.0f); dw[n++] = F32Bits(1.0f); dw[n++] = F32Bits(1.0f);
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        uint32_t s0 = (VIRGL_BLIT_MASK_RGBA & 0xFF) | ((VIRGL_TEX_FILTER_NEAREST & 0x3) << 8);
        dw[n++] = VirglCmd0(VIRGL_CCMD_BLIT, 0, 21);
        dw[n++] = s0; dw[n++] = 0; dw[n++] = 0;
        dw[n++] = TST_RT; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
        dw[n++] = 32; dw[n++] = 32; dw[n++] = 0; dw[n++] = tw; dw[n++] = th; dw[n++] = 1;
        dw[n++] = TST_TEX; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = tw; dw[n++] = th; dw[n++] = 1;
        sub->size = n * 4;
        if (!CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP)))
        { SerialPuts("virtio_gpu: comp self-test compose submit failed\n"); return; }
    }
    if (!TransferFromHost3D(TST_CTX, TST_RT, sdim, sdim))
    { SerialPuts("virtio_gpu: comp self-test readback failed\n"); return; }

    uint32_t bg = sread[0 * sdim + 0];       // (0,0): blue
    uint32_t fg = sread[64 * sdim + 64];     // (64,64): inside 32..128 → green
    auto cB = [](uint32_t p){ return p & 0xFF; };
    auto cG = [](uint32_t p){ return (p >> 8) & 0xFF; };
    auto cR = [](uint32_t p){ return (p >> 16) & 0xFF; };
    bool bgBlue  = (cB(bg) > 0xC0) && (cR(bg) < 0x40) && (cG(bg) < 0x40);
    bool fgGreen = (cG(fg) > 0xC0) && (cR(fg) < 0x40) && (cB(fg) < 0x40);
    bool ok = bgBlue && fgGreen;
    SerialPrintf("virtio_gpu: compositor self-test bg=0x%08x fg=0x%08x (scattered tex) -> %s\n",
                 bg, fg, ok ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// VirGL DRAW pipeline bring-up (Milestone: textured-quad composition).
//
// The compositor today composes with BLIT (a host-side copy/resolve). The DRAW
// path renders textured quads through the real GL pipeline (vertex+fragment
// TGSI shaders, blend state, DRAW_VBO), which is what enables per-window
// constant opacity and generalises to Venus/Vulkan + real hardware later.
//
// This is a STANDALONE self-test ladder that proves the pipeline in isolation
// before the compositor is ever switched to it. Each rung renders into its own
// readback RT with a DISTINCT expected output, so a failure localises to one
// subsystem rather than collapsing to an ambiguous "black frame":
//   M1 solid-colour FS   → proves shader compile + vertex elements + VBO +
//                          viewport + framebuffer bind + DRAW_VBO.
//   M2 uv-as-colour FS   → proves vertex attribute / uv interpolation.
//   M3 textured FS       → proves sampler view + sampler state + TEX + swizzle.
// (M4 alpha-blend + the compositor swap come once these three are green.)
// ---------------------------------------------------------------------------

// Create a PIPE_BUFFER resource (vertex/constant data) as a raw byte buffer.
static bool ResourceCreateBuffer(uint32_t resId, uint32_t bind, uint32_t byteLen)
{
    auto* req = reinterpret_cast<VirtioGpuResourceCreate3D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    req->resource_id = resId;
    req->target      = PIPE_BUFFER;
    req->format      = VIRGL_FORMAT_R8_UNORM;
    req->bind        = bind;
    req->width       = byteLen;
    req->height      = 1;
    req->depth       = 1;
    req->array_size  = 1;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

// Transfer a buffer resource's full backing to the host (1-D box).
static bool TransferToHostBuffer(uint32_t ctxId, uint32_t resId, uint32_t byteLen)
{
    auto* req = reinterpret_cast<VirtioGpuTransferHost3D*>(g_cmdBuf + CMD_REQ_OFF);
    memset(req, 0, sizeof(*req));
    req->hdr.type     = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    req->hdr.ctx_id   = ctxId;
    req->box.w        = byteLen;
    req->box.h        = 1;
    req->box.d        = 1;
    req->offset       = 0;
    req->resource_id  = resId;
    req->stride       = byteLen;
    req->layer_stride = byteLen;
    return CmdRespOk(SubmitCommand(sizeof(*req), CMD_RESP_CAP));
}

// Create a virgl shader object from TGSI *text* (virglrenderer parses it with
// tgsi_text_translate host-side). Single-pass framing: 5 header dwords (handle,
// type, offlen=byteLen, num_tokens, num_so_outputs=0) followed by the NUL-
// terminated text zero-padded to a dword boundary. num_tokens is intentionally
// generous (host callocs num_tokens+10 tokens; over-estimating only allocates
// more host memory). Returns true only if the SUBMIT_3D response is OK, giving
// an in-guest signal for a malformed/oversized command (not a TGSI compile
// error, which virglrenderer reports only on the host log).
static bool CreateShaderObj(uint32_t ctxId, uint32_t handle, uint32_t shaderType,
                            const char* text)
{
    uint32_t slen = 0; while (text[slen]) slen++;
    uint32_t strBytes = slen + 1;            // include the NUL
    uint32_t strDw    = (strBytes + 3) / 4;  // dwords occupied by the text

    auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
    memset(sub, 0, sizeof(*sub));
    sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = ctxId;
    uint32_t* dw = ComposeDwBase(); uint32_t n = 0;
    dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, 5 + strDw);
    dw[n++] = handle;
    dw[n++] = shaderType;        // PIPE_SHADER_VERTEX / PIPE_SHADER_FRAGMENT
    dw[n++] = strBytes;          // offlen: total length, no continuation bit
    dw[n++] = strBytes;          // num_tokens (generous over-estimate)
    dw[n++] = 0;                 // num_so_outputs (no stream-out)
    uint8_t* dst = reinterpret_cast<uint8_t*>(&dw[n]);
    for (uint32_t i = 0; i < strBytes; ++i)      dst[i] = static_cast<uint8_t>(text[i]);
    for (uint32_t i = strBytes; i < strDw * 4; ++i) dst[i] = 0;
    n += strDw;
    sub->size = n * 4;
    return CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP));
}

// Pass-through vertex shader: clip-space position from IN[0], uv from IN[1].
// M1 fragment shader: constant red (proves the pipeline draws at all).
static const char* kDrawFS_Solid =
    "FRAG\n"
    "DCL OUT[0], COLOR\n"
    "IMM[0] FLT32 { 1.0000, 0.0000, 0.0000, 1.0000}\n"
    "MOV OUT[0], IMM[0]\n"
    "END\n";

// M2 fragment shader: output the interpolated uv as colour (r=u, g=v).
static const char* kDrawFS_UV =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "MOV OUT[0], IN[0]\n"
    "END\n";

// Drives one rung of the DRAW ladder against a fresh readback RT and returns the
// centre pixel. ctx/objects are created once by the caller; `fsHandle` selects
// the fragment shader bound for this rung, and `texView`/`sampState` (non-zero
// for the textured rung) bind a sampler view + state before the draw.
static bool VirtioGpuDrawSelfTest()
{
    if (!(g_gpu3dFeatures & VIRTIO_GPU_F_VIRGL)) return false;

    const uint32_t DCTX = 4;
    const uint32_t sdim = 256, sbytes = sdim * sdim * 4;

    if (!CtxCreate(DCTX, VIRTIO_GPU_CAPSET_VIRGL))
    { SerialPuts("virtio_gpu: draw-test ctx create failed\n"); return false; }

    // Readback RTs (one per rung) — contiguous backing so we can read pixels.
    // Rung 3 (M4) tests alpha blending; rung 4 (M5) tests an arbitrary sub-rect
    // quad whose vertices are built at runtime via NdcBits (the M6 geometry path).
    const uint32_t RT[5] = { 200, 201, 202, 203, 204 };
    const uint32_t SURF[5] = { 83, 84, 85, 92, 93 };
    uint32_t* rd[5] = {};
    for (int i = 0; i < 5; ++i)
    {
        PhysicalAddress p = PmmAllocPages(AlignUp(sbytes, 4096) / 4096, MemTag::Device, KernelPid);
        if (!p) { SerialPuts("virtio_gpu: draw-test RT alloc failed\n"); return false; }
        rd[i] = reinterpret_cast<uint32_t*>(PhysToVirt(p).raw());
        memset(rd[i], 0, sbytes);
        if (!ResourceCreate3D(RT[i], VIRGL_FORMAT_B8G8R8X8_UNORM,
                              VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW, sdim, sdim) ||
            !ResourceAttachBackingContig(RT[i], p.raw(), sbytes) ||
            !CtxAttachResource(DCTX, RT[i]))
        { SerialPrintf("virtio_gpu: draw-test RT%d setup failed\n", i); return false; }
    }

    // Fullscreen quad as a triangle strip: {pos.xy, uv.xy} per vertex. Stored as
    // raw IEEE-754 bit patterns (not float values) so no runtime FP is emitted
    // under the kernel's -mno-sse build — the GPU interprets the bits as floats.
    constexpr uint32_t FN1 = 0xBF800000u;  // -1.0f
    constexpr uint32_t FP1 = 0x3F800000u;  //  1.0f
    constexpr uint32_t F00 = 0x00000000u;  //  0.0f
    static const uint32_t quadBits[16] = {
        FN1, FN1, F00, F00,   // (-1,-1) uv(0,0)
        FP1, FN1, FP1, F00,   // ( 1,-1) uv(1,0)
        FN1, FP1, F00, FP1,   // (-1, 1) uv(0,1)
        FP1, FP1, FP1, FP1,   // ( 1, 1) uv(1,1)
    };
    const uint32_t VBUF = 210, vbytes = sizeof(quadBits);
    {
        PhysicalAddress vp = PmmAllocPages(AlignUp(vbytes, 4096) / 4096, MemTag::Device, KernelPid);
        if (!vp) { SerialPuts("virtio_gpu: draw-test vbuf alloc failed\n"); return false; }
        uint32_t* vbacking = reinterpret_cast<uint32_t*>(PhysToVirt(vp).raw());
        for (uint32_t i = 0; i < 16; ++i) vbacking[i] = quadBits[i];
        if (!ResourceCreateBuffer(VBUF, VIRGL_BIND_VERTEX_BUFFER, vbytes) ||
            !ResourceAttachBackingContig(VBUF, vp.raw(), vbytes) ||
            !CtxAttachResource(DCTX, VBUF) ||
            !TransferToHostBuffer(DCTX, VBUF, vbytes))
        { SerialPuts("virtio_gpu: draw-test vbuf setup failed\n"); return false; }
    }

    // M5 vertex buffer: an arbitrary sub-rect quad (64,64)-(192,192) inside the
    // 256x256 RT, with positions built AT RUNTIME from integer pixel coords via
    // NdcBits — exercising the exact pixel->NDC path the compositor will use in
    // M6 (no literal coords, no runtime FP). uv spans the full texture.
    const uint32_t VBUF2 = 213;
    {
        const int32_t x0 = 64, y0 = 64, x1 = 192, y1 = 192;
        uint32_t nx0 = NdcBits(x0, sdim), ny0 = NdcBits(y0, sdim);
        uint32_t nx1 = NdcBits(x1, sdim), ny1 = NdcBits(y1, sdim);
        const uint32_t F00b = 0x00000000u, FP1b = 0x3F800000u;
        uint32_t sub[16] = {
            nx0, ny0, F00b, F00b,
            nx1, ny0, FP1b, F00b,
            nx0, ny1, F00b, FP1b,
            nx1, ny1, FP1b, FP1b,
        };
        PhysicalAddress vp = PmmAllocPages(AlignUp(vbytes, 4096) / 4096, MemTag::Device, KernelPid);
        if (!vp) { SerialPuts("virtio_gpu: draw-test vbuf2 alloc failed\n"); return false; }
        uint32_t* vbacking = reinterpret_cast<uint32_t*>(PhysToVirt(vp).raw());
        for (uint32_t i = 0; i < 16; ++i) vbacking[i] = sub[i];
        if (!ResourceCreateBuffer(VBUF2, VIRGL_BIND_VERTEX_BUFFER, vbytes) ||
            !ResourceAttachBackingContig(VBUF2, vp.raw(), vbytes) ||
            !CtxAttachResource(DCTX, VBUF2) ||
            !TransferToHostBuffer(DCTX, VBUF2, vbytes))
        { SerialPuts("virtio_gpu: draw-test vbuf2 setup failed\n"); return false; }
    }

    // A solid-magenta sampler texture for the M3/M5 rungs (distinct from
    // clear-blue and M1 solid-red, so a correct sample is unambiguous).
    const uint32_t TEX = 211, texDim = 4, texBytes = texDim * texDim * 4;
    {
        PhysicalAddress tp = PmmAllocPages(1, MemTag::Device, KernelPid);
        if (!tp) { SerialPuts("virtio_gpu: draw-test tex alloc failed\n"); return false; }
        uint32_t* tpx = reinterpret_cast<uint32_t*>(PhysToVirt(tp).raw());
        for (uint32_t i = 0; i < texDim * texDim; ++i) tpx[i] = 0xFFFF00FFu; // opaque magenta
        if (!ResourceCreate3D(TEX, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_SAMPLER_VIEW, texDim, texDim) ||
            !ResourceAttachBackingContig(TEX, tp.raw(), texBytes) ||
            !CtxAttachResource(DCTX, TEX) ||
            !TransferToHost3D(DCTX, TEX, 0, 0, texDim, texDim, texDim, texDim))
        { SerialPuts("virtio_gpu: draw-test tex setup failed\n"); return false; }
    }

    // A half-alpha green texture for the M4 (blend) rung: green at alpha 0x80.
    // Drawn over a red-cleared RT with SRC_ALPHA blending, the centre should
    // come back ~50/50 red+green (olive), proving hardware alpha blend.
    const uint32_t TEX2 = 212;
    {
        PhysicalAddress tp = PmmAllocPages(1, MemTag::Device, KernelPid);
        if (!tp) { SerialPuts("virtio_gpu: draw-test tex2 alloc failed\n"); return false; }
        uint32_t* tpx = reinterpret_cast<uint32_t*>(PhysToVirt(tp).raw());
        for (uint32_t i = 0; i < texDim * texDim; ++i) tpx[i] = 0x8000FF00u; // green, alpha 0x80
        if (!ResourceCreate3D(TEX2, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_SAMPLER_VIEW, texDim, texDim) ||
            !ResourceAttachBackingContig(TEX2, tp.raw(), texBytes) ||
            !CtxAttachResource(DCTX, TEX2) ||
            !TransferToHost3D(DCTX, TEX2, 0, 0, texDim, texDim, texDim, texDim))
        { SerialPuts("virtio_gpu: draw-test tex2 setup failed\n"); return false; }
    }

    // Shaders.
    const uint32_t VS = 77, FS_SOLID = 78, FS_UV = 79, FS_TEX = 90;
    bool sv = CreateShaderObj(DCTX, VS, PIPE_SHADER_VERTEX, kDrawVS);
    bool s1 = CreateShaderObj(DCTX, FS_SOLID, PIPE_SHADER_FRAGMENT, kDrawFS_Solid);
    bool s2 = CreateShaderObj(DCTX, FS_UV, PIPE_SHADER_FRAGMENT, kDrawFS_UV);
    bool s3 = CreateShaderObj(DCTX, FS_TEX, PIPE_SHADER_FRAGMENT, kDrawFS_Tex);
    bool shOk = sv && s1 && s2 && s3;
    SerialPrintf("virtio_gpu: draw-test shader create -> %s\n", shOk ? "ok" : "SUBMIT-ERR");

    // Pipeline state objects + surfaces, all in one SUBMIT_3D.
    const uint32_t BLEND = 73, RAST = 74, DSA = 75, VE = 76, SAMP = 87, SVIEW = 88;
    const uint32_t BLEND_ON = 91, SVIEW2 = 89;
    {
        auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
        memset(sub, 0, sizeof(*sub));
        sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = DCTX;
        uint32_t* dw = ComposeDwBase(); uint32_t n = 0;

        // Surfaces over each readback RT.
        for (int i = 0; i < 5; ++i) {
            dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
            dw[n++] = SURF[i]; dw[n++] = RT[i]; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
            dw[n++] = 0; dw[n++] = 0;
        }
        // Blend: no blend, write all channels (colormask RGBA = 0xF at bits 27-30).
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11);
        dw[n++] = BLEND; dw[n++] = 0; dw[n++] = 0;
        dw[n++] = (0xFu << 27);              // S2[0]: colormask only
        for (int i = 1; i < 8; ++i) dw[n++] = 0;
        // Blend (enabled): standard src-alpha over — out = src*srcA + dst*(1-srcA).
        // S2[0] fields: RT_BLEND_ENABLE(0) | RGB_FUNC(1..3)=ADD |
        // RGB_SRC_FACTOR(4..8)=SRC_ALPHA | RGB_DST_FACTOR(9..13)=INV_SRC_ALPHA |
        // ALPHA_FUNC(14..16)=ADD | ALPHA_SRC(17..21)=SRC_ALPHA |
        // ALPHA_DST(22..26)=INV_SRC_ALPHA | COLORMASK(27..30)=RGBA.
        {
            uint32_t blendS2 = 1u
                        | (PIPE_BLEND_ADD << 1)
                        | (PIPE_BLENDFACTOR_SRC_ALPHA << 4)
                        | (PIPE_BLENDFACTOR_INV_SRC_ALPHA << 9)
                        | (PIPE_BLEND_ADD << 14)
                        | (PIPE_BLENDFACTOR_SRC_ALPHA << 17)
                        | (PIPE_BLENDFACTOR_INV_SRC_ALPHA << 22)
                        | (0xFu << 27);
            dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11);
            dw[n++] = BLEND_ON; dw[n++] = 0; dw[n++] = 0;
            dw[n++] = blendS2;
            for (int i = 1; i < 8; ++i) dw[n++] = 0;
        }
        // Rasterizer: depth-clip + half-pixel-center, no culling.
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9);
        dw[n++] = RAST;
        dw[n++] = (1u << 1) | (1u << 29);    // S0: DEPTH_CLIP | HALF_PIXEL_CENTER
        dw[n++] = F32Bits(1.0f);             // point size
        dw[n++] = 0;                         // sprite coord enable
        dw[n++] = 0;                         // S3
        dw[n++] = F32Bits(1.0f);             // line width
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; // offset units/scale/clamp
        // DSA: depth/stencil/alpha all off.
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5);
        dw[n++] = DSA; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        // Vertex elements: pos @0 (R32G32_FLOAT), uv @8 (R32G32_FLOAT), both vb 0.
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 9);
        dw[n++] = VE;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_R32G32_FLOAT;
        dw[n++] = 8; dw[n++] = 0; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_R32G32_FLOAT;
        // Sampler state: nearest filter, clamp-to-edge wrap (for the M3 rung).
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_STATE, 9);
        dw[n++] = SAMP; dw[n++] = 0;         // S0: all-zero = nearest, wrap repeat
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; // lod/border
        // Sampler view over the magenta texture. The last dword is the channel
        // swizzle: identity R→R,G→G,B→B,A→A = (0)|(1<<3)|(2<<6)|(3<<9). A zero
        // swizzle would map every channel to X (red), turning magenta into white.
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 6);
        dw[n++] = SVIEW; dw[n++] = TEX; dw[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0x688u;
        // Sampler view over the half-alpha green texture (M4 blend rung).
        dw[n++] = VirglCmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 6);
        dw[n++] = SVIEW2; dw[n++] = TEX2; dw[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0x688u;
        sub->size = n * 4;
        bool ok = CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP));
        SerialPrintf("virtio_gpu: draw-test object create -> %s\n", ok ? "ok" : "SUBMIT-ERR");
    }

    // Run one rung in two separately-submitted, separately-read-back phases so a
    // failure localises despite QEMU swallowing host-side virgl errors (the
    // submit response is OK even when virglrenderer rejects a command):
    //   Phase A: SET_FRAMEBUFFER_STATE + CLEAR(blue) → proves surface + clear +
    //            readback on THIS RT. If A is not blue, the surface/RT/readback
    //            is the problem, not the draw.
    //   Phase B: viewport + binds + vbuf + DRAW → the actual pipeline.
    auto runRung = [&](int rt, uint32_t fs, bool textured, uint32_t sview,
                       uint32_t blendObj, uint32_t clrR, uint32_t clrG, uint32_t clrB,
                       uint32_t vbuf) {
        // --- Phase A: clear only ---
        {
            auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
            memset(sub, 0, sizeof(*sub));
            sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = DCTX;
            uint32_t* dw = ComposeDwBase(); uint32_t n = 0;
            dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
            dw[n++] = 1; dw[n++] = 0; dw[n++] = SURF[rt];
            dw[n++] = VirglCmd0(VIRGL_CCMD_CLEAR, 0, 8);
            dw[n++] = PIPE_CLEAR_COLOR0;
            dw[n++] = clrR; dw[n++] = clrG; dw[n++] = clrB; dw[n++] = F32Bits(1.0f);
            dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
            sub->size = n * 4;
            bool sok = CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP));
            bool xok = TransferFromHost3D(DCTX, RT[rt], sdim, sdim);
            uint32_t cpx = rd[rt][sdim / 2 * sdim + sdim / 2];
            SerialPrintf("virtio_gpu:  rung%d clearA submit=%d xfer=%d centre=0x%08x\n",
                         rt, sok, xok, cpx);
        }
        // --- Phase B: full draw pipeline ---
        {
            auto* sub = reinterpret_cast<VirtioGpuCmdSubmit*>(g_cmdBuf + CMD_REQ_OFF);
            memset(sub, 0, sizeof(*sub));
            sub->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D; sub->hdr.ctx_id = DCTX;
            uint32_t* dw = ComposeDwBase(); uint32_t n = 0;
            dw[n++] = VirglCmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
            dw[n++] = 1; dw[n++] = 0; dw[n++] = SURF[rt];
            dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
            dw[n++] = 0;
            dw[n++] = F32Bits(128.0f); dw[n++] = F32Bits(128.0f); dw[n++] = F32Bits(1.0f);
            dw[n++] = F32Bits(128.0f); dw[n++] = F32Bits(128.0f); dw[n++] = F32Bits(0.0f);
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1);       dw[n++] = blendObj;
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_RASTERIZER, 1);  dw[n++] = RAST;
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_DSA, 1);         dw[n++] = DSA;
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 1); dw[n++] = VE;
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = VS; dw[n++] = PIPE_SHADER_VERTEX;
            dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = fs; dw[n++] = PIPE_SHADER_FRAGMENT;
            if (textured) {
                dw[n++] = VirglCmd0(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 3);
                dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = sview;
                dw[n++] = VirglCmd0(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 3);
                dw[n++] = PIPE_SHADER_FRAGMENT; dw[n++] = 0; dw[n++] = SAMP;
            }
            dw[n++] = VirglCmd0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
            dw[n++] = 16; dw[n++] = 0; dw[n++] = vbuf;
            dw[n++] = VirglCmd0(VIRGL_CCMD_DRAW_VBO, 0, 12);
            dw[n++] = 0;  dw[n++] = 4;  dw[n++] = PIPE_PRIM_TRIANGLE_STRIP; dw[n++] = 0;
            dw[n++] = 1;  dw[n++] = 0;  dw[n++] = 0;  dw[n++] = 0;
            dw[n++] = 0;  dw[n++] = 0;  dw[n++] = 3;  dw[n++] = 0;
            sub->size = n * 4;
            bool sok = CmdRespOk(SubmitCommand(sizeof(VirtioGpuCmdSubmit) + n * 4, CMD_RESP_CAP));
            bool xok = TransferFromHost3D(DCTX, RT[rt], sdim, sdim);
            uint32_t cpx = rd[rt][sdim / 2 * sdim + sdim / 2];
            SerialPrintf("virtio_gpu:  rung%d drawB  submit=%d xfer=%d centre=0x%08x\n",
                         rt, sok, xok, cpx);
        }
    };

    constexpr uint32_t CLR0 = 0x00000000u, CLR1 = 0x3F800000u;  // 0.0f, 1.0f bits
    runRung(0, FS_SOLID, false, 0,      BLEND,    CLR0, CLR0, CLR1, VBUF);   // clear blue
    runRung(1, FS_UV,    false, 0,      BLEND,    CLR0, CLR0, CLR1, VBUF);   // clear blue
    runRung(2, FS_TEX,   true,  SVIEW,  BLEND,    CLR0, CLR0, CLR1, VBUF);   // clear blue
    runRung(3, FS_TEX,   true,  SVIEW2, BLEND_ON, CLR1, CLR0, CLR0, VBUF);   // clear red, blend green
    runRung(4, FS_TEX,   true,  SVIEW,  BLEND,    CLR0, CLR0, CLR1, VBUF2);  // clear blue, sub-rect quad

    auto cR = [](uint32_t p){ return (p >> 16) & 0xFF; };
    auto cG = [](uint32_t p){ return (p >> 8) & 0xFF; };
    auto cB = [](uint32_t p){ return p & 0xFF; };
    uint32_t c = sdim / 2 * sdim + sdim / 2;   // centre pixel index

    uint32_t p0 = rd[0][c];
    bool m1 = cR(p0) > 0xC0 && cG(p0) < 0x40 && cB(p0) < 0x40;          // red
    SerialPrintf("virtio_gpu: DRAW M1 solid centre=0x%08x -> %s\n", p0, m1 ? "PASS" : "FAIL");

    uint32_t p1 = rd[1][c];
    bool m2 = cR(p1) > 0x40 && cG(p1) > 0x40 && cB(p1) < 0x40;          // uv≈(0.5,0.5)
    SerialPrintf("virtio_gpu: DRAW M2 uv centre=0x%08x -> %s\n", p1, m2 ? "PASS" : "FAIL");

    uint32_t p2 = rd[2][c];
    bool m3 = cR(p2) > 0xC0 && cG(p2) < 0x40 && cB(p2) > 0xC0;          // magenta
    SerialPrintf("virtio_gpu: DRAW M3 tex centre=0x%08x -> %s\n", p2, m3 ? "PASS" : "FAIL");

    // M4: half-alpha green over red clear → ~50/50 red+green (olive), with both
    // channels mid-range and blue near zero. Proves hardware src-alpha blend.
    uint32_t p3 = rd[3][c];
    bool m4 = cR(p3) > 0x50 && cR(p3) < 0xB0 && cG(p3) > 0x50 && cG(p3) < 0xB0 && cB(p3) < 0x40;
    SerialPrintf("virtio_gpu: DRAW M4 blend centre=0x%08x -> %s\n", p3, m4 ? "PASS" : "FAIL");

    // M5: sub-rect (64,64)-(192,192) of the 256 RT, vertices built at runtime via
    // NdcBits. Centre (128,128) must be inside → magenta; a corner sample (32,32)
    // must be OUTSIDE → the blue clear. Proves the integer pixel→NDC geometry the
    // compositor will use, and that an arbitrary quad lands at the right pixels.
    uint32_t pIn  = rd[4][128 * sdim + 128];
    uint32_t pOut = rd[4][32 * sdim + 32];
    bool inMag  = cR(pIn) > 0xC0 && cG(pIn) < 0x40 && cB(pIn) > 0xC0;   // magenta
    bool outBlu = cB(pOut) > 0xC0 && cR(pOut) < 0x40 && cG(pOut) < 0x40; // blue
    bool m5 = inMag && outBlu;
    SerialPrintf("virtio_gpu: DRAW M5 rect in=0x%08x out=0x%08x -> %s\n",
                 pIn, pOut, m5 ? "PASS" : "FAIL");

    return m1 && m2 && m3 && m4 && m5;
}

static int VirtioGpuModuleInit()
{
    SerialPuts("virtio_gpu: init\n");

    PciDevice dev;
    if (!PciFindDevice(0x1AF4, 0x1050, dev)) // virtio-gpu-pci (modern)
    {
        SerialPuts("virtio_gpu: device 1af4:1050 not found\n");
        return -1;
    }
    SerialPrintf("virtio_gpu: found at %02x:%02x.%x\n", dev.bus, dev.dev, dev.fn);

    PciEnableMemSpace(dev);
    PciEnableBusMaster(dev); // device DMAs command buffers + framebuffer backing
    DisableMsix(dev);

    VirtioPciCap caps[5] = {};
    if (!FindVirtioCaps(dev, caps))
    {
        SerialPuts("virtio_gpu: missing virtio PCI caps\n");
        return -1;
    }

    g_commonCfg = MapBar(dev, caps[VIRTIO_PCI_CAP_COMMON_CFG].bar,
                         caps[VIRTIO_PCI_CAP_COMMON_CFG].offset,
                         caps[VIRTIO_PCI_CAP_COMMON_CFG].length);
    g_notifyCfg = MapBar(dev, caps[VIRTIO_PCI_CAP_NOTIFY_CFG].bar,
                         caps[VIRTIO_PCI_CAP_NOTIFY_CFG].offset,
                         caps[VIRTIO_PCI_CAP_NOTIFY_CFG].length);
    g_isrCfg    = MapBar(dev, caps[VIRTIO_PCI_CAP_ISR_CFG].bar,
                         caps[VIRTIO_PCI_CAP_ISR_CFG].offset,
                         caps[VIRTIO_PCI_CAP_ISR_CFG].length);
    g_deviceCfg = MapBar(dev, caps[VIRTIO_PCI_CAP_DEVICE_CFG].bar,
                         caps[VIRTIO_PCI_CAP_DEVICE_CFG].offset,
                         caps[VIRTIO_PCI_CAP_DEVICE_CFG].length);
    if (!g_commonCfg || !g_notifyCfg || !g_isrCfg || !g_deviceCfg)
    {
        SerialPuts("virtio_gpu: failed to map config regions\n");
        return -1;
    }

    // Reset, then ACK + DRIVER.
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, 0);
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // Feature negotiation.
    //   Page 0 (low 32 bits): virtio-gpu device features. We surface and accept
    //   the 3D-transport features the GPU-accel roadmap needs — VIRGL (3D),
    //   RESOURCE_BLOB (host-visible memory) and CONTEXT_INIT (per-context capset,
    //   required by Venus) — but only those the device actually offers. On a
    //   plain virtio-gpu-pci (2D) device none are offered and we fall through to
    //   the existing 2D behaviour unchanged. Accepting VIRGL does NOT disturb the
    //   2D path (GET_DISPLAY_INFO / RESOURCE_CREATE_2D / SET_SCANOUT still work);
    //   it only unlocks the capset query + future 3D contexts.
    //   Page 1 (bits 32-63): transport features; accept VIRTIO_F_VERSION_1 only.
    mmio_write32(g_commonCfg, VIRTIO_COMMON_DFSELECT, 0);
    uint32_t devF0 = mmio_read32(g_commonCfg, VIRTIO_COMMON_DF);
    const uint32_t wanted0 = VIRTIO_GPU_F_VIRGL | VIRTIO_GPU_F_RESOURCE_BLOB |
                             VIRTIO_GPU_F_CONTEXT_INIT;
    g_gpu3dFeatures = devF0 & wanted0;
    SerialPrintf("virtio_gpu: device features page0=0x%x [virgl=%d edid=%d uuid=%d blob=%d ctx_init=%d]\n",
                 devF0,
                 (devF0 & VIRTIO_GPU_F_VIRGL)         ? 1 : 0,
                 (devF0 & VIRTIO_GPU_F_EDID)          ? 1 : 0,
                 (devF0 & VIRTIO_GPU_F_RESOURCE_UUID) ? 1 : 0,
                 (devF0 & VIRTIO_GPU_F_RESOURCE_BLOB) ? 1 : 0,
                 (devF0 & VIRTIO_GPU_F_CONTEXT_INIT)  ? 1 : 0);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GFSELECT, 0);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GF, g_gpu3dFeatures);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_DFSELECT, 1);
    uint32_t devF1 = mmio_read32(g_commonCfg, VIRTIO_COMMON_DF);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GFSELECT, 1);
    mmio_write32(g_commonCfg, VIRTIO_COMMON_GF, devF1 & 0x01);

    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    if (!(mmio_read8(g_commonCfg, VIRTIO_COMMON_STATUS) & VIRTIO_STATUS_FEATURES_OK))
    {
        SerialPuts("virtio_gpu: FEATURES_OK rejected by device\n");
        return -1;
    }

    // Set up controlq (queue 0).
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SELECT, 0);
    g_queueSize = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_SIZE);
    if (g_queueSize == 0)
    {
        SerialPuts("virtio_gpu: controlq size 0\n");
        return -1;
    }
    if (g_queueSize > MAX_QUEUE_SIZE)
    {
        mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_SIZE, MAX_QUEUE_SIZE);
        g_queueSize = MAX_QUEUE_SIZE;
    }
    { uint16_t p = 1; while (p * 2u <= g_queueSize) p *= 2; g_queueSize = p; }

    g_queueNotifyOff = mmio_read16(g_commonCfg, VIRTIO_COMMON_Q_NOTIFY_OFF);

    if (!AllocControlQueue())
    {
        SerialPuts("virtio_gpu: controlq alloc failed\n");
        return -1;
    }

    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_DESC,  g_descPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_AVAIL, g_availPhys);
    mmio_write64(g_commonCfg, VIRTIO_COMMON_Q_USED,  g_usedPhys);
    mmio_write16(g_commonCfg, VIRTIO_COMMON_Q_ENABLE, 1);

    mmio_write8(g_commonCfg, VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    SerialPrintf("virtio_gpu: controlq size %u, notifyOff %u, mult %u\n",
                 g_queueSize, g_queueNotifyOff, g_notifyMultiplier);

    if (!QueryDisplayInfo())
    {
        SerialPuts("virtio_gpu: GET_DISPLAY_INFO failed\n");
        return -1;
    }

    // Phase A: enumerate host 3D capsets (no-op on a 2D device). This proves the
    // VIRGL negotiation + capset-query path that the Venus/DRM bring-up builds on.
    QueryCapsets();

    // Milestone 1 of the VirGL fixed-function compositor path: a shader-free
    // GPU-clear self-test that proves the 3D context/resource/SUBMIT_3D/transfer
    // path end-to-end (and lights the taskbar 3D badge on success). Runs on both
    // primary and secondary heads; no-op on a 2D device.
    VirtioGpu3DSelfTest();

    // Validate the real compositor ops (scattered-backed texture + upload + blit
    // compose, readback-verified). Proves the path the window compositor uses.
    VirtioGpuCompositorSelfTest();

    // Validate the DRAW (textured-quad) pipeline in isolation — the next-step
    // composition path (per-window opacity, Venus/Vulkan-portable). Readback-
    // verified ladder; does not touch the live compositor.
    VirtioGpuDrawSelfTest();

    // Take over the display only when we are the PRIMARY device — i.e. a
    // VGA-class device (virtio-vga, PCI subclass 0x00) that provided the boot
    // GOP. A secondary virtio-gpu-pci head (display-other, subclass 0x80) is
    // left idle so the default stdvga+bochs boot is unaffected.
    uint8_t baseClass = PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x0B);
    uint8_t subClass  = PciConfigRead8(dev.bus, dev.dev, dev.fn, 0x0A);
    bool isPrimaryVga = (baseClass == 0x03 && subClass == 0x00);

    if (isPrimaryVga)
    {
        if (!VirtioGpuTakeOverDisplay())
        {
            SerialPuts("virtio_gpu: display takeover failed\n");
            return -1;
        }

        // If the host 3D path is live, set up the GPU compositor (persistent
        // context + scanout render-target at display size) and register its ops.
        // The window compositor uses them only when BROOK_COMPOSITE=gpu; until a
        // GPU frame is presented, scanout 0 stays on the 2D framebuffer, so this
        // is inert by default.
        if (g_gpu3dWorks && g_fbW && g_fbH)
        {
            if (SetupGpuCompositor(g_fbW, g_fbH))
            {
                GpuCompositorRegister(&g_gpuCompositorOps);
                // Same proven 3D path also backs per-process app GL contexts.
                GpuAppRegister(&g_gpuAppOps);
            }
        }
    }
    else
    {
        KPrintf("virtio_gpu: secondary head (class %02x:%02x) — not driving display\n",
                baseClass, subClass);
    }
    return 0;
}

static void VirtioGpuModuleExit()
{
    SerialPuts("virtio_gpu: exit\n");
}

DECLARE_MODULE("virtio_gpu", VirtioGpuModuleInit, VirtioGpuModuleExit,
               "virtio-gpu 2D display driver (PCI 1af4:1050)");
