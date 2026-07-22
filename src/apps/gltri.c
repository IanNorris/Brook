// gltri.c — first Brook-native GPU app. Draws a hardware-accelerated, gradient-
// shaded triangle into its own window using the per-process virgl GPU syscalls
// (0xB10-0xB16). Proves the whole userspace GPU path end to end:
//   create window -> GPU context -> render-target resource backed by the window
//   VFB -> upload a vertex buffer -> submit a virgl command stream (pipeline
//   objects + shaders, then framebuffer/clear/draw) -> transfer the rendered
//   pixels from the host into the window -> signal the compositor.
//
// The virgl command encoding mirrors the kernel virtio-gpu driver's proven DRAW
// self-test (VirglCmd0 layout, vertex-element layout, the pass-through vertex
// shader + uv->colour fragment shader). Unlike the kernel, this is musl
// userspace, so real floating point is available for building the vertex data
// and viewport.

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

// --- Window-manager syscalls (see wmtest.c). ---
#define BROOK_WM_CREATE_WINDOW   506
#define BROOK_WM_SIGNAL_DIRTY    508
#define BROOK_WM_POP_INPUT       510

// --- GPU app syscalls (Brook extension block 0xB10+). ---
#define BROOK_GPU_CTX_CREATE     0xB10  // () -> ctxId
#define BROOK_GPU_RESOURCE_CREATE 0xB11 // (ctx,format,bind,w,h) -> resId
#define BROOK_GPU_ATTACH_WINDOW  0xB12  // (ctx,resId,wmId)
#define BROOK_GPU_SUBMIT         0xB13  // (ctx,cmdPtr,nDwords)
#define BROOK_GPU_TRANSFER       0xB14  // (ctx,resId,dir,w,h)  dir 0=to_host 1=from_host
#define BROOK_GPU_CTX_DESTROY    0xB15  // (ctx)
#define BROOK_GPU_UPLOAD_BUFFER  0xB16  // (ctx,srcPtr,bytes) -> resId

// --- virgl constants (mirror src/drivers/virtio_gpu/virtio_gpu_mod.cpp). ---
#define VIRGL_FORMAT_B8G8R8X8_UNORM  2
#define VIRGL_FORMAT_R32G32_FLOAT    29
#define VIRGL_BIND_RENDER_TARGET     (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW      (1u << 3)
#define PIPE_CLEAR_COLOR0            (1u << 2)

#define VIRGL_CCMD_CREATE_OBJECT        1
#define VIRGL_CCMD_BIND_OBJECT          2
#define VIRGL_CCMD_SET_VIEWPORT_STATE   4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS   6
#define VIRGL_CCMD_CLEAR                7
#define VIRGL_CCMD_DRAW_VBO             8
#define VIRGL_CCMD_BIND_SHADER          31

#define VIRGL_OBJECT_BLEND             1
#define VIRGL_OBJECT_RASTERIZER        2
#define VIRGL_OBJECT_DSA               3
#define VIRGL_OBJECT_SHADER            4
#define VIRGL_OBJECT_VERTEX_ELEMENTS   5
#define VIRGL_OBJECT_SURFACE           8

#define PIPE_SHADER_VERTEX             0
#define PIPE_SHADER_FRAGMENT           1
#define PIPE_PRIM_TRIANGLES            4

// Object/surface handles are per-context and chosen freely by this app.
#define H_BLEND   1
#define H_RAST    2
#define H_DSA     3
#define H_VE      4
#define H_SURF    5
#define H_VS      6
#define H_FS      7

struct wm_create_out {
    uint32_t wm_id;
    uint32_t vfb_stride;
    uint64_t vfb_user;
};

struct wm_input_evt {
    uint8_t type, scan, ascii, mods;
    int16_t x, y;
    uint32_t reserved;
};

// Pass-through vertex shader (clip-space position from IN[0], uv from IN[1]) and
// a fragment shader that emits the interpolated uv as colour (r=u, g=v) — the
// triangle becomes a smooth GPU-interpolated gradient.
static const char* kVS =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "MOV OUT[0], IN[0]\n"
    "MOV OUT[1], IN[1]\n"
    "END\n";
static const char* kFS =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "MOV OUT[0], IN[0]\n"
    "END\n";

static inline uint32_t f32bits(float v) {
    uint32_t u; memcpy(&u, &v, sizeof(u)); return u;
}
static inline uint32_t virgl_cmd0(uint32_t cmd, uint32_t obj, uint32_t len) {
    return cmd | (obj << 8) | (len << 16);
}

// Emit a CREATE_OBJECT SHADER command (handle, type, TGSI text) into dw[]; return
// the new dword count.
static uint32_t emit_shader(uint32_t* dw, uint32_t n, uint32_t handle,
                            uint32_t type, const char* text) {
    uint32_t slen = (uint32_t)strlen(text);
    uint32_t strBytes = slen + 1;            // include NUL
    uint32_t strDw = (strBytes + 3) / 4;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, 5 + strDw);
    dw[n++] = handle;
    dw[n++] = type;
    dw[n++] = strBytes;      // offlen: total length, no continuation
    dw[n++] = strBytes;      // num_tokens (generous over-estimate)
    dw[n++] = 0;             // num_so_outputs
    uint8_t* dst = (uint8_t*)&dw[n];
    for (uint32_t i = 0; i < strBytes; ++i)        dst[i] = (uint8_t)text[i];
    for (uint32_t i = strBytes; i < strDw * 4; ++i) dst[i] = 0;
    n += strDw;
    return n;
}

int main(void) {
    const uint16_t W = 400, H = 300;
    struct wm_create_out win;
    if (syscall(BROOK_WM_CREATE_WINDOW, (long)W, (long)H,
                (long)"GPU triangle", (long)&win) != 0)
        return 1;

    long ctx = syscall(BROOK_GPU_CTX_CREATE);
    if (ctx <= 0) return 2;

    // Render-target texture, dimensions == window content, backed by the window
    // VFB so a from-host transfer lands rendered pixels straight in the window.
    long rt = syscall(BROOK_GPU_RESOURCE_CREATE, ctx,
                      (long)VIRGL_FORMAT_B8G8R8X8_UNORM,
                      (long)(VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW),
                      (long)W, (long)H);
    if (rt <= 0) return 3;
    if (syscall(BROOK_GPU_ATTACH_WINDOW, ctx, rt, (long)win.wm_id) != 0)
        return 4;

    // Vertex buffer: 3 vertices, each {pos.x, pos.y, uv.x, uv.y} (16-byte stride,
    // matching the vertex-element layout below). The host renders y-up but the
    // window VFB is y-down, so NDC y is negated here to make the apex point up
    // on screen. uv at the corners drives the gradient: apex orange (r=.5,g=0),
    // bottom-left green (g=1), bottom-right yellow (r=1,g=1).
    float verts[3 * 4] = {
        0.0f, -0.8f,   0.5f, 0.0f,   // apex (top on screen)
       -0.8f,  0.8f,   0.0f, 1.0f,   // bottom-left
        0.8f,  0.8f,   1.0f, 1.0f,   // bottom-right
    };
    long vbuf = syscall(BROOK_GPU_UPLOAD_BUFFER, ctx,
                        (long)verts, (long)sizeof(verts));
    if (vbuf <= 0) return 5;

    // --- Submit 1: pipeline state objects + shaders. ---
    static uint32_t dw[2048];
    uint32_t n = 0;
    // Blend: no blend, write all channels (colormask RGBA at bits 27-30).
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11);
    dw[n++] = H_BLEND; dw[n++] = 0; dw[n++] = 0;
    dw[n++] = (0xFu << 27);
    for (int i = 1; i < 8; ++i) dw[n++] = 0;
    // Rasterizer: depth-clip + half-pixel-center, no culling.
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9);
    dw[n++] = H_RAST;
    dw[n++] = (1u << 1) | (1u << 29);
    dw[n++] = f32bits(1.0f);
    dw[n++] = 0; dw[n++] = 0;
    dw[n++] = f32bits(1.0f);
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
    // DSA: depth/stencil/alpha off.
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5);
    dw[n++] = H_DSA; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
    // Vertex elements: pos @0 (RG32F), uv @8 (RG32F), both from vertex buffer 0.
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 9);
    dw[n++] = H_VE;
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_R32G32_FLOAT;
    dw[n++] = 8; dw[n++] = 0; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_R32G32_FLOAT;
    // Surface over the render-target resource.
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    dw[n++] = H_SURF; dw[n++] = (uint32_t)rt; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    dw[n++] = 0; dw[n++] = 0;
    // Shaders.
    n = emit_shader(dw, n, H_VS, PIPE_SHADER_VERTEX, kVS);
    n = emit_shader(dw, n, H_FS, PIPE_SHADER_FRAGMENT, kFS);
    if (syscall(BROOK_GPU_SUBMIT, ctx, (long)dw, (long)n) != 0)
        return 6;

    // --- Submit 2: framebuffer + viewport + binds + clear + draw. ---
    n = 0;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    dw[n++] = 1; dw[n++] = 0; dw[n++] = H_SURF;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7);
    dw[n++] = 0;
    dw[n++] = f32bits(W / 2.0f); dw[n++] = f32bits(H / 2.0f); dw[n++] = f32bits(1.0f);
    dw[n++] = f32bits(W / 2.0f); dw[n++] = f32bits(H / 2.0f); dw[n++] = f32bits(0.0f);
    dw[n++] = virgl_cmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1);          dw[n++] = H_BLEND;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_RASTERIZER, 1);     dw[n++] = H_RAST;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_DSA, 1);            dw[n++] = H_DSA;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 1);dw[n++] = H_VE;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = H_VS; dw[n++] = PIPE_SHADER_VERTEX;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_BIND_SHADER, 0, 2); dw[n++] = H_FS; dw[n++] = PIPE_SHADER_FRAGMENT;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
    dw[n++] = 16; dw[n++] = 0; dw[n++] = (uint32_t)vbuf;     // stride, offset, res
    // Clear to dark blue so the triangle is unmistakably on a GPU-cleared field.
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CLEAR, 0, 8);
    dw[n++] = PIPE_CLEAR_COLOR0;
    dw[n++] = f32bits(0.05f); dw[n++] = f32bits(0.07f); dw[n++] = f32bits(0.18f); dw[n++] = f32bits(1.0f);
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
    // Draw the triangle (3 vertices).
    dw[n++] = virgl_cmd0(VIRGL_CCMD_DRAW_VBO, 0, 12);
    dw[n++] = 0;  dw[n++] = 3;  dw[n++] = PIPE_PRIM_TRIANGLES; dw[n++] = 0;
    dw[n++] = 1;  dw[n++] = 0;  dw[n++] = 0;  dw[n++] = 0;
    dw[n++] = 0;  dw[n++] = 0;  dw[n++] = 2;  dw[n++] = 0;
    if (syscall(BROOK_GPU_SUBMIT, ctx, (long)dw, (long)n) != 0)
        return 7;

    // Pull the rendered RT into the window VFB and show it.
    if (syscall(BROOK_GPU_TRANSFER, ctx, rt, 1L /*from_host*/, (long)W, (long)H) != 0)
        return 8;
    syscall(BROOK_WM_SIGNAL_DIRTY, win.wm_id);

    // Idle: drain input so the window stays responsive; re-present on demand.
    struct wm_input_evt ev[16];
    for (;;) {
        long got = syscall(BROOK_WM_POP_INPUT, win.wm_id, (long)ev, 16L);
        if (got > 0) syscall(BROOK_WM_SIGNAL_DIRTY, win.wm_id);
        usleep(16000);
    }
    return 0;
}
