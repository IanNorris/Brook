// Compile the actual builder with a hosted scene and guarded request region.
// Protocol/object constants mirror the driver; these tests verify construction
// bounds and command framing, not host rendering semantics.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../../drivers/virtio_gpu/control_storage.h"
using namespace brook;
namespace {
unsigned failures;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;}}while(0)
constexpr uint32_t VIRTIO_GPU_CMD_SUBMIT_3D=0x207;
constexpr uint32_t CMD_REQ_OFF=0,CMD_RESP_OFF=15*4096;
struct VirtioGpuCmdSubmit {VirtioGpuCtrlHdr hdr;uint32_t size,padding;};
static_assert(sizeof(VirtioGpuCmdSubmit)==32);
constexpr uint32_t fullCapacity=(CMD_RESP_OFF-sizeof(VirtioGpuCmdSubmit))/4;
uint32_t COMPOSE_DWORD_CAPACITY=fullCapacity;
alignas(4096) uint8_t bytes[16*4096];
bool g_controlQueuePoisoned=false,g_blurReady=true;
uint32_t g_blurAView=71,g_blurBView=72;
constexpr uint32_t QUAD_SAMPLE_BLUR=1,QUAD_PREMULTIPLIED=2;
struct DrawQuadRec {uint32_t blend,flags,sview;};
DrawQuadRec g_drawQuads[256];uint32_t g_drawQuadCount;
uint32_t g_blurBarrierCount,g_blurBarrierIdx[16],g_blurBarrierStrength[16];
uint32_t* ComposeDwBase(uint8_t* b){return reinterpret_cast<uint32_t*>(b+sizeof(VirtioGpuCmdSubmit));}
uint32_t VirglCmd0(uint32_t cmd,uint32_t obj,uint32_t len){return cmd|(obj<<8)|(len<<16);}
uint32_t F32Bits(float f){uint32_t b;std::memcpy(&b,&f,4);return b;}
uint32_t IntToF32Bits(int32_t n){return F32Bits(static_cast<float>(n));}
uint32_t GpuViewportHalfBits(int32_t n){return F32Bits(static_cast<float>(n)/2);}
constexpr uint32_t BLUR_A_SURF=13;
constexpr uint32_t BLUR_B_SURF=14;
constexpr uint32_t COMP_BLURVTX_RES=7;
constexpr uint32_t COMP_BLUR_A_RES=5;
constexpr uint32_t COMP_BLUR_H=270;
constexpr uint32_t COMP_BLUR_W=480;
constexpr uint32_t COMP_CTX_ID=2;
constexpr uint32_t COMP_DRAW_VBUF_RES=13;
constexpr uint32_t COMP_SCANOUT_RES=16;
constexpr uint32_t COMP_SCANOUT_SURF=1;
constexpr uint32_t DRAW_BLEND=2;
constexpr uint32_t DRAW_DSA=5;
constexpr uint32_t DRAW_FS=9;
constexpr uint32_t DRAW_FS_BLURH=10;
constexpr uint32_t DRAW_FS_BLURV=11;
constexpr uint32_t DRAW_FS_PM=16;
constexpr uint32_t DRAW_RAST=4;
constexpr uint32_t DRAW_SAMP=7;
constexpr uint32_t DRAW_SAMP_LIN=12;
constexpr uint32_t DRAW_VE=6;
constexpr uint32_t DRAW_VS=8;
constexpr uint32_t PIPE_CLEAR_COLOR0=1u << 2;
constexpr uint32_t PIPE_PRIM_TRIANGLE_STRIP=5;
constexpr uint32_t PIPE_SHADER_FRAGMENT=1;
constexpr uint32_t PIPE_SHADER_VERTEX=0;
constexpr uint32_t VIRGL_BLIT_MASK_RGBA=0xF;
constexpr uint32_t VIRGL_CCMD_BIND_OBJECT=2;
constexpr uint32_t VIRGL_CCMD_BIND_SAMPLER_STATES=18;
constexpr uint32_t VIRGL_CCMD_BIND_SHADER=31;
constexpr uint32_t VIRGL_CCMD_BLIT=16;
constexpr uint32_t VIRGL_CCMD_CLEAR=7;
constexpr uint32_t VIRGL_CCMD_DRAW_VBO=8;
constexpr uint32_t VIRGL_CCMD_SET_FRAMEBUFFER_STATE=5;
constexpr uint32_t VIRGL_CCMD_SET_SAMPLER_VIEWS=10;
constexpr uint32_t VIRGL_CCMD_SET_VERTEX_BUFFERS=6;
constexpr uint32_t VIRGL_CCMD_SET_VIEWPORT_STATE=4;
constexpr uint32_t VIRGL_FORMAT_B8G8R8X8_UNORM=2;
constexpr uint32_t VIRGL_OBJECT_BLEND=1;
constexpr uint32_t VIRGL_OBJECT_DSA=3;
constexpr uint32_t VIRGL_OBJECT_RASTERIZER=2;
constexpr uint32_t VIRGL_OBJECT_VERTEX_ELEMENTS=5;
constexpr uint32_t VIRGL_TEX_FILTER_LINEAR=1;
#include "../../drivers/virtio_gpu/compose_packet.inc"
void guard(uint32_t capacity) {
    const auto begin=sizeof(VirtioGpuCmdSubmit)+capacity*4;
    for(unsigned i=begin;i<sizeof(bytes);++i)CHECK(bytes[i]==0xa5);
}
}
int main() {
    GpuCommandStorage storage;storage.bytes=bytes;storage.physical=0x100000;
    for(unsigned i=0;i<256;++i)g_drawQuads[i]={i%2?DRAW_BLEND:99u,(i%2?QUAD_PREMULTIPLIED:QUAD_SAMPLE_BLUR),i+100};
    for(unsigned i=0;i<16;++i){g_blurBarrierIdx[i]=i*16;g_blurBarrierStrength[i]=16;}
    g_drawQuadCount=256;g_blurBarrierCount=16;
    std::memset(bytes,0xa5,sizeof(bytes));
    const auto count=BuildComposePacket(storage,1920,1080);
    CHECK(count && count<=CMD_RESP_OFF);guard(fullCapacity);
    CHECK(reinterpret_cast<VirtioGpuCmdSubmit*>(bytes)->size==count-sizeof(VirtioGpuCmdSubmit));
    // Every emitted packet must terminate on a declared command boundary.
    auto* dw=ComposeDwBase(bytes);uint32_t at=0,n=(count-sizeof(VirtioGpuCmdSubmit))/4;
    while(at<n){const auto length=1+(dw[at]>>16);CHECK(length<=n-at);if(length>n-at)break;at+=length;}
    CHECK(at==n);
    // Lower the logical capacity to exercise the first rejected quad boundary,
    // keeping physical response/guard bytes untouched. Then rebuild normally.
    g_blurBarrierCount=0;g_drawQuadCount=1;
    std::memset(bytes,0xa5,sizeof(bytes));const auto one=BuildComposePacket(storage,1920,1080);CHECK(one);
    COMPOSE_DWORD_CAPACITY=(one-sizeof(VirtioGpuCmdSubmit))/4+31;
    g_drawQuadCount=2;std::memset(bytes,0xa5,sizeof(bytes));
    CHECK(!BuildComposePacket(storage,1920,1080));guard(COMPOSE_DWORD_CAPACITY);
    COMPOSE_DWORD_CAPACITY=fullCapacity;CHECK(BuildComposePacket(storage,1920,1080));
    storage.state=GpuCommandStorageState::Published;std::memset(bytes,0xa5,sizeof(bytes));
    CHECK(!BuildComposePacket(storage,1920,1080));guard(0);
    storage.state=GpuCommandStorageState::Writable;g_controlQueuePoisoned=true;
    CHECK(!BuildComposePacket(storage,1920,1080));guard(0);
    std::printf("GPU_COMPOSE_PACKET %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
