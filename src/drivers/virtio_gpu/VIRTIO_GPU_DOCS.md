# virtio-gpu 2D display driver — design & recon notes

Status: **Phase 1 (recon) complete — no driver code yet.**

A loadable display driver for the QEMU **virtio-gpu** device, sitting behind the
kernel's existing `DisplayOps` abstraction. The immediate goal is **2D scanout**
(faster presentation on QEMU + the driver *shape* that Venus/Vulkan will reuse),
**not** 3D/virgl acceleration.

## Why this, why now
- Today the compositor flip copies up to ~8 MB of **uncacheable MMIO** into the
  emulated stdvga BAR every frame (`compositor.cpp` — "the expensive part: MMIO
  writes are uncacheable"). virtio-gpu keeps the backbuffer in **normal guest
  RAM** and issues one `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` per damage rect,
  removing that tax. NB: faster *presentation*, **not** accelerated composition —
  the CPU still composites; real accel needs virgl/Venus.
- The driver skeleton (modern virtio-1.0 PCI transport, virtqueue command
  submission, the resource/scanout model) is exactly what a future **Venus**
  driver reuses. Build it once, cheaply, with 2D.

Explicitly **not** a portability play: virtio-gpu is QEMU/virtio-specific (bare
metal → GOP; VMware → SVGA; VBox/Hyper-V → their own). The portable lever is the
`DisplayOps` abstraction + one driver per environment; stdvga/GOP remain the
fallback.

## Environment (recon facts)
- QEMU **10.2.2**; device `virtio-gpu-pci` (alias `virtio-gpu`) is present.
  Props: `edid=on` (default), `max_outputs=1`, `xres=1280`, `yres=800`,
  `blob`, `hostmem`/`max_hostmem` (blob resources — not used in 2D path).
- PCI ID: vendor **0x1AF4**, modern device **0x1050** (non-transitional).
  (`virtio-gpu-pci` is modern-only — no legacy I/O-port BAR, unlike virtio_blk.)
- `run-qemu.sh` currently passes **no** `-vga` flag; q35 defaults to QEMU stdvga
  (`1234:1111`, driven by `bochs_display`). For bring-up we'll run with
  `-vga none -device virtio-gpu-pci` (Phase 5+); the bochs/GOP path stays as the
  no-virtio-gpu fallback.

## Template mapping (what to clone, and from where)
| Need                                   | Source to clone                              |
|----------------------------------------|----------------------------------------------|
| Modern virtio-1.0 PCI cap parse        | `virtio_input_mod.cpp` `FindVirtioCaps` (~477) |
| BAR mapping                            | `virtio_input_mod.cpp` `MapBar` (~537)       |
| Reset → feature-negotiate → DRIVER_OK  | `virtio_input_mod.cpp` init (~649–752)       |
| Virtqueue alloc (desc/avail/used pages)| `virtio_input_mod.cpp` `AllocEventQueue` (~211) |
| Notify (offset × multiplier MMIO)      | `virtio_input_mod.cpp` `NotifyQueue` (~268)  |
| Poll used-ring without IRQs            | `virtio_blk.cpp` `SubmitRequest` spin-wait   |
| `DisplayOps` registration + remap      | `bochs_display_mod.cpp` (whole file, ~220 ln)|
| Build registration                     | `src/kernel/drivers/CMakeLists.txt` `add_driver_module` (use `-mgeneral-regs-only -mno-sse -mno-sse2 -mfpmath=` like the other virtio modules) |

virtio-gpu is **virtio-1.0 only**, so the `virtio_input` modern path is the
correct template — do **not** copy `virtio_blk`'s legacy I/O-port approach.

## virtio-gpu command protocol (controlq) — structs to define
Two virtqueues: **controlq (0)** — used here; **cursorq (1)** — deferred (SW cursor).
Each control command = a chain of **2 descriptors**: descriptor 0 device-readable
(request), descriptor 1 device-writable (response), exactly like the blk header/
status split.

```c
// All little-endian. Common header on every request & response.
struct virtio_gpu_ctrl_hdr {
    uint32_t type;        // command (req) or response code (resp)
    uint32_t flags;       // VIRTIO_GPU_FLAG_FENCE (bit 0) if using fences
    uint64_t fence_id;
    uint32_t ctx_id;      // 3D only — 0 for 2D
    uint8_t  ring_idx;
    uint8_t  padding[3];
};

struct virtio_gpu_rect { uint32_t x, y, width, height; };
```

Command / response codes:
```
VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106  // ("bind pages")
VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
VIRTIO_GPU_RESP_OK_NODATA              0x1100
VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
```

Per-command payloads (after the header):
```c
// GET_DISPLAY_INFO: req = hdr only. resp:
#define VIRTIO_GPU_MAX_SCANOUTS 16
struct virtio_gpu_resp_display_info {
    virtio_gpu_ctrl_hdr hdr;            // type = RESP_OK_DISPLAY_INFO
    struct { virtio_gpu_rect r; uint32_t enabled; uint32_t flags; }
        pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

// RESOURCE_CREATE_2D
struct { virtio_gpu_ctrl_hdr hdr; uint32_t resource_id; uint32_t format;
         uint32_t width; uint32_t height; };

// RESOURCE_ATTACH_BACKING (header) + nr_entries × mem_entry
struct { virtio_gpu_ctrl_hdr hdr; uint32_t resource_id; uint32_t nr_entries; };
struct virtio_gpu_mem_entry { uint64_t addr; uint32_t length; uint32_t padding; };

// SET_SCANOUT
struct { virtio_gpu_ctrl_hdr hdr; virtio_gpu_rect r;
         uint32_t scanout_id; uint32_t resource_id; };

// TRANSFER_TO_HOST_2D
struct { virtio_gpu_ctrl_hdr hdr; virtio_gpu_rect r;
         uint64_t offset; uint32_t resource_id; uint32_t padding; };

// RESOURCE_FLUSH
struct { virtio_gpu_ctrl_hdr hdr; virtio_gpu_rect r;
         uint32_t resource_id; uint32_t padding; };
```

## Pixel format
Brook's framebuffer is **Bgr8** (`boot_protocol.h`: Blue[7:0] Green[15:8]
Red[23:16] X[31:24] — memory bytes B,G,R,X), which is virtio-gpu
**`VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM` = 2**. (Verify against the GOP-reported
format at init; handle the `Rgb8` case → `R8G8B8X8_UNORM` = 134 if it ever
differs. Default assumption: format 2.)

## The one real integration nuance — backing pages
`RESOURCE_ATTACH_BACKING` needs the **physical pages of the compositor
backbuffer** (`g_backBuffer`, a kernel `VmmAllocPages` allocation). It is **not
guaranteed physically contiguous**, so build the `virtio_gpu_mem_entry[]` list by
walking the backbuffer page-by-page with `VmmVirtToPhys(KernelPageTable, …)`,
coalescing physically-adjacent pages into fewer entries. One scanout resource =
the whole framebuffer; SW composition into its backing is unchanged.

## `DisplayOps::Flush` change (Phase 2, no behaviour change)
Current signature: `void (*Flush)()`. Extend to carry a damage rect:
`void (*Flush)(uint32_t minY, uint32_t maxY)` (or a small rect struct). The
compositor already tracks `g_dirtyMinY/g_dirtyMaxY`; pass those at the flip site
(~`compositor.cpp:2000`). Update the two existing impls (`GopFlush`,
`BochsFlush`) to ignore the rect (still no-ops). virtio-gpu's `Flush` issues
`TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` for `(0, minY)–(width, maxY)`.

## Phase plan (SQL todos `vgpu-*`)
1. **vgpu-recon** ← this doc. ✅
2. **vgpu-flush-rect** — extend `DisplayOps::Flush` + 2 impls + compositor call site (no behaviour change).
3. **vgpu-skeleton** — module: PCI detect, modern caps, controlq, feature negotiate; verify `GET_DISPLAY_INFO`.
4. **vgpu-resource** — `RESOURCE_CREATE_2D` + backing pages + `SET_SCANOUT`; test fill on screen.
5. **vgpu-flush-wire** — transfer+flush on damage rect; `DisplayRegister`; `CompositorRemap` to backing; live desktop.
6. **vgpu-modeset** — `SetMode` → recreate resource + set_scanout.
7. **vgpu-verify** — A/B perf (COMPOSITOR flip-ms stats), screenshot, flip `run-qemu.sh` default, finalise this doc.
8. **vgpu-venus-notes** — document Venus/3D extension points (3D contexts, fences, blob resources).

## Out of scope (deferred)
HW cursor (cursorq), multi-scanout/EDID/hotplug, 3D/virgl/Venus, and a shared
virtio/virtqueue library refactor (4 drivers currently inline their own virtqueue
code — recommend separately; do not fold in here).
