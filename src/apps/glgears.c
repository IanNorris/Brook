// glgears.c — a Brook-native "glxgears": three hardware-accelerated, spinning,
// interlocking gears rendered through the per-process virgl GPU syscalls
// (0xB10-0xB16), the same proven path as gltri.c.
//
// Animation strategy: the BufferUpload syscall creates a new host resource per
// call (no in-place update, global cap of 64 buffers), so re-uploading geometry
// every frame would exhaust resources. Instead we pre-bake FRAMES whole-scene
// vertex buffers ONCE at startup — each holding all three gears rotated to that
// frame's angles — then the render loop simply cycles SET_VERTEX_BUFFERS through
// them. FRAMES * (360/FRAMES) = 360 degrees, so the loop is seamless (gear 1
// completes one revolution, gears 2/3 complete two).
//
// Geometry is generated on the CPU (musl gives us real floating point + libm):
// each gear is a triangle fan from its centre out to a toothed outline, with a
// per-vertex RGBA colour and gentle radial shading for depth. The virgl command
// encoding mirrors gltri.c / the kernel driver's DRAW self-test, extended with a
// second vertex element (RGBA colour) and a colour-passthrough shader.

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/syscall.h>

// --- Window-manager syscalls (see wmtest.c / gltri.c). ---
#define BROOK_WM_CREATE_WINDOW   506
#define BROOK_WM_SIGNAL_DIRTY    508
#define BROOK_WM_POP_INPUT       510

// --- GPU app syscalls (Brook extension block 0xB10+). ---
#define BROOK_GPU_CTX_CREATE      0xB10  // () -> ctxId
#define BROOK_GPU_RESOURCE_CREATE 0xB11  // (ctx,format,bind,w,h) -> resId
#define BROOK_GPU_ATTACH_WINDOW   0xB12  // (ctx,resId,wmId)
#define BROOK_GPU_SUBMIT          0xB13  // (ctx,cmdPtr,nDwords)
#define BROOK_GPU_TRANSFER        0xB14  // (ctx,resId,dir,w,h) dir 0=to_host 1=from_host
#define BROOK_GPU_CTX_DESTROY     0xB15  // (ctx)
#define BROOK_GPU_UPLOAD_BUFFER   0xB16  // (ctx,srcPtr,bytes) -> resId

// --- virgl constants (mirror src/drivers/virtio_gpu/virtio_gpu_mod.cpp). ---
#define VIRGL_FORMAT_B8G8R8X8_UNORM       2
#define VIRGL_FORMAT_R32G32_FLOAT         29
#define VIRGL_FORMAT_R32G32B32A32_FLOAT   31
#define VIRGL_BIND_RENDER_TARGET     (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW      (1u << 3)
#define PIPE_CLEAR_COLOR0            (1u << 2)

#define VIRGL_CCMD_CREATE_OBJECT         1
#define VIRGL_CCMD_BIND_OBJECT           2
#define VIRGL_CCMD_SET_VIEWPORT_STATE    4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS    6
#define VIRGL_CCMD_CLEAR                 7
#define VIRGL_CCMD_DRAW_VBO              8
#define VIRGL_CCMD_BIND_SHADER          31

#define VIRGL_OBJECT_BLEND             1
#define VIRGL_OBJECT_RASTERIZER        2
#define VIRGL_OBJECT_DSA               3
#define VIRGL_OBJECT_VERTEX_ELEMENTS   5
#define VIRGL_OBJECT_SURFACE           8

#define PIPE_SHADER_VERTEX             0
#define PIPE_SHADER_FRAGMENT           1
#define PIPE_PRIM_TRIANGLES            4

// Per-context object/surface handles (chosen freely by this app).
#define H_BLEND   1
#define H_RAST    2
#define H_DSA     3
#define H_VE      4
#define H_SURF    5
#define H_VS      6
#define H_FS      7

#define W 520
#define H 520
#define FRAMES 16                  // rotation frames pre-baked; 48*7.5deg = 360
#define VERT_FLOATS 6              // x,y, r,g,b,a
#define MAX_VERTS 2048             // generous upper bound for 3 gears

struct wm_create_out { uint32_t wm_id; uint32_t vfb_stride; uint64_t vfb_user; };
struct wm_input_evt  { uint8_t type, scan, ascii, mods; int16_t x, y; uint32_t reserved; };

// A pass-through vertex shader (clip-space pos from IN[0], colour from IN[1])
// and a fragment shader that emits the interpolated colour directly.
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

static inline uint32_t f32bits(float v) { uint32_t u; memcpy(&u, &v, sizeof(u)); return u; }
static inline uint32_t virgl_cmd0(uint32_t cmd, uint32_t obj, uint32_t len) {
    return cmd | (obj << 8) | (len << 16);
}

static uint32_t emit_shader(uint32_t* dw, uint32_t n, uint32_t handle,
                            uint32_t type, const char* text) {
    uint32_t slen = (uint32_t)strlen(text);
    uint32_t strBytes = slen + 1;
    uint32_t strDw = (strBytes + 3) / 4;
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, 4 /*SHADER*/, 5 + strDw);
    dw[n++] = handle;
    dw[n++] = type;
    dw[n++] = strBytes;
    dw[n++] = strBytes;
    dw[n++] = 0;
    uint8_t* dst = (uint8_t*)&dw[n];
    for (uint32_t i = 0; i < strBytes; ++i)         dst[i] = (uint8_t)text[i];
    for (uint32_t i = strBytes; i < strDw * 4; ++i) dst[i] = 0;
    return n + strDw;
}

// Scene layout (model space) — glxgears-style positions and gear ratios.
struct GearDef {
    float cx, cy;          // centre in model space
    float pitch;           // pitch radius
    float tooth;           // tooth depth (tip - root span)
    int   teeth;
    float r, g, b;         // base colour
    float phase;           // angle offset (radians)
    float spin;            // angular speed multiplier vs the master angle
};

static const struct GearDef GEARS[3] = {
    // centre        pitch tooth teeth   colour (r,g,b)     phase            spin
    { -3.0f, -2.0f,  4.0f, 0.9f, 20,  0.85f, 0.12f, 0.10f,  0.0f,            +1.0f }, // red
    {  3.1f, -2.0f,  2.0f, 0.9f, 10,  0.15f, 0.80f, 0.20f, -9.0f*M_PI/180,  -2.0f }, // green
    { -3.1f,  4.2f,  2.0f, 0.9f, 10,  0.25f, 0.30f, 1.00f,-25.0f*M_PI/180,  -2.0f }, // blue
};

// Scene -> NDC mapping (the scene roughly spans model x[-7.7,5.8], y[-6.7,6.9]).
#define SCENE_CX  (-0.95f)
#define SCENE_CY  ( 0.10f)
#define SCENE_SCALE 0.118f

// Append one vertex (model point + colour) to v[], y-flipped into NDC (the
// window VFB is y-down; the host renders y-up).
static uint32_t put_vert(float* v, uint32_t n, float mx, float my,
                         float r, float g, float b) {
    v[n*VERT_FLOATS + 0] = (mx - SCENE_CX) * SCENE_SCALE;
    v[n*VERT_FLOATS + 1] = -(my - SCENE_CY) * SCENE_SCALE;
    v[n*VERT_FLOATS + 2] = r;
    v[n*VERT_FLOATS + 3] = g;
    v[n*VERT_FLOATS + 4] = b;
    v[n*VERT_FLOATS + 5] = 1.0f;
    return n + 1;
}

// Build the whole 3-gear scene at master angle `ang` (radians) into v[];
// returns the vertex count.
static uint32_t build_scene(float* v, float ang) {
    uint32_t n = 0;
    for (int gi = 0; gi < 3; ++gi) {
        const struct GearDef* G = &GEARS[gi];
        float a   = G->phase + ang * G->spin;
        float rr  = G->pitch - G->tooth * 0.5f;   // root radius
        float rt  = G->pitch + G->tooth * 0.5f;   // tip radius
        int   T   = G->teeth;
        float da  = (float)(2.0 * M_PI) / (T * 4);

        // Outline points: per tooth root,tip,tip,root (4 per tooth).
        float px[ (20*4) ], py[ (20*4) ], pshade[ (20*4) ];
        int np = 0;
        for (int i = 0; i < T; ++i) {
            float base = (float)(2.0 * M_PI) * i / T;
            const float radii[4]  = { rr, rt, rt, rr };
            for (int j = 0; j < 4; ++j) {
                float phi = base + j * da + a;
                float radius = radii[j];
                px[np] = G->cx + radius * cosf(phi);
                py[np] = G->cy + radius * sinf(phi);
                // Brighter at the tips for a sense of depth.
                pshade[np] = 0.55f + 0.45f * ((radius - rr) / (rt - rr));
                np++;
            }
        }
        // Triangle fan from the gear centre to consecutive outline points.
        for (int k = 0; k < np; ++k) {
            int k2 = (k + 1) % np;
            float sh = (pshade[k] + pshade[k2]) * 0.5f;
            n = put_vert(v, n, G->cx, G->cy, G->r*0.5f, G->g*0.5f, G->b*0.5f);
            n = put_vert(v, n, px[k],  py[k],  G->r*pshade[k],  G->g*pshade[k],  G->b*pshade[k]);
            n = put_vert(v, n, px[k2], py[k2], G->r*pshade[k2], G->g*pshade[k2], G->b*pshade[k2]);
            (void)sh;
        }
    }
    return n;
}

int main(void) {
    struct wm_create_out win;
    if (syscall(BROOK_WM_CREATE_WINDOW, (long)W, (long)H,
                (long)"GPU gears", (long)&win) != 0)
        return 1;

    long ctx = syscall(BROOK_GPU_CTX_CREATE);
    if (ctx <= 0) return 2;

    // Render target == window content, backed by the window VFB so a from-host
    // transfer lands rendered pixels straight in the window.
    long rt = syscall(BROOK_GPU_RESOURCE_CREATE, ctx,
                      (long)VIRGL_FORMAT_B8G8R8X8_UNORM,
                      (long)(VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW),
                      (long)W, (long)H);
    if (rt <= 0) return 3;
    if (syscall(BROOK_GPU_ATTACH_WINDOW, ctx, rt, (long)win.wm_id) != 0)
        return 4;

    // Pre-bake the rotation frames: one whole-scene vertex buffer per frame.
    static float scene[MAX_VERTS * VERT_FLOATS];
    long      frameBuf[FRAMES];
    uint32_t  frameVerts = 0;
    for (int f = 0; f < FRAMES; ++f) {
        float ang = (float)(2.0 * M_PI) * f / FRAMES;
        frameVerts = build_scene(scene, ang);
        if (frameVerts > MAX_VERTS) return 5;
        long vb = syscall(BROOK_GPU_UPLOAD_BUFFER, ctx, (long)scene,
                          (long)(frameVerts * VERT_FLOATS * sizeof(float)));
        if (vb <= 0) { return 6; }
        frameBuf[f] = vb;
    }

    // --- Submit 1: pipeline state objects + shaders (once). ---
    static uint32_t dw[4096];
    uint32_t n = 0;
    // Blend: no blend, write RGBA (colormask at bits 27-30).
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11);
    dw[n++] = H_BLEND; dw[n++] = 0; dw[n++] = 0;
    dw[n++] = (0xFu << 27);
    for (int i = 1; i < 8; ++i) dw[n++] = 0;
    // Rasterizer: depth-clip + half-pixel-center, no culling (gears are 2D fans
    // wound both ways, so culling would drop half the triangles).
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
    // Vertex elements: pos @0 (RG32F), colour @8 (RGBA32F), both from buffer 0.
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 9);
    dw[n++] = H_VE;
    dw[n++] = 0; dw[n++] = 0; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_R32G32_FLOAT;
    dw[n++] = 8; dw[n++] = 0; dw[n++] = 0; dw[n++] = VIRGL_FORMAT_R32G32B32A32_FLOAT;
    // Surface over the render target.
    dw[n++] = virgl_cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    dw[n++] = H_SURF; dw[n++] = (uint32_t)rt; dw[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    dw[n++] = 0; dw[n++] = 0;
    n = emit_shader(dw, n, H_VS, PIPE_SHADER_VERTEX, kVS);
    n = emit_shader(dw, n, H_FS, PIPE_SHADER_FRAGMENT, kFS);
    if (syscall(BROOK_GPU_SUBMIT, ctx, (long)dw, (long)n) != 0)
        return 7;

    // --- Submit 2: framebuffer + viewport + binds (once; state persists). ---
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
    if (syscall(BROOK_GPU_SUBMIT, ctx, (long)dw, (long)n) != 0)
        return 8;

    // --- Render loop: cycle the pre-baked frames. ---
    struct wm_input_evt ev[16];
    int frame = 0;
    for (;;) {
        n = 0;
        // Point the vertex buffer at this frame's pre-baked geometry.
        dw[n++] = virgl_cmd0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
        dw[n++] = VERT_FLOATS * sizeof(float);    // stride (24)
        dw[n++] = 0;                               // offset
        dw[n++] = (uint32_t)frameBuf[frame];       // resource
        // Clear to a dark slate field.
        dw[n++] = virgl_cmd0(VIRGL_CCMD_CLEAR, 0, 8);
        dw[n++] = PIPE_CLEAR_COLOR0;
        dw[n++] = f32bits(0.06f); dw[n++] = f32bits(0.07f); dw[n++] = f32bits(0.10f); dw[n++] = f32bits(1.0f);
        dw[n++] = 0; dw[n++] = 0; dw[n++] = 0;
        // Draw the whole scene.
        dw[n++] = virgl_cmd0(VIRGL_CCMD_DRAW_VBO, 0, 12);
        dw[n++] = 0;  dw[n++] = frameVerts;  dw[n++] = PIPE_PRIM_TRIANGLES; dw[n++] = 0;
        dw[n++] = 1;  dw[n++] = 0;  dw[n++] = 0;  dw[n++] = 0;
        dw[n++] = 0;  dw[n++] = 0;  dw[n++] = 2;  dw[n++] = 0;
        if (syscall(BROOK_GPU_SUBMIT, ctx, (long)dw, (long)n) != 0)
            { break; }
        if (syscall(BROOK_GPU_TRANSFER, ctx, rt, 1L /*from_host*/, (long)W, (long)H) != 0)
            { break; }
        syscall(BROOK_WM_SIGNAL_DIRTY, win.wm_id);

        // Drain input so the window stays responsive; close on Escape.
        long got = syscall(BROOK_WM_POP_INPUT, win.wm_id, (long)ev, 16L);
        for (long i = 0; i < got; ++i)
            if (ev[i].type == 1 && ev[i].scan == 1) { // key down, Escape
                syscall(BROOK_GPU_CTX_DESTROY, ctx);
                return 0;
            }

        frame = (frame + 1) % FRAMES;
        usleep(33000);                              // ~30 fps
    }

    syscall(BROOK_GPU_CTX_DESTROY, ctx);
    return 0;
}
