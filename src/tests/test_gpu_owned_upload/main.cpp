// Execute the production owned-upload and resource-control bodies. Host commands
// model asynchronous source reads; a returned ACK cannot release those reads.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../kernel/src/gpu_compositor.h"
#include "../../drivers/virtio_gpu/control_storage.h"
#include "../../drivers/virtio_gpu/retirement_protocol.h"
using namespace brook;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line%d %s\n",__LINE__,#x);std::abort();}}while(0)
namespace {
struct GpuTexture {bool owned=true,quarantined=false,uploadBackingAttached=false;uint32_t w=4,h=3,format=1,resId=77;};
static GpuTexture texture;
static bool g_ownedCaptureReady=true,g_compReady=true,g_drawReady=true;
static bool g_compFrameAssembling,g_controlQueuePoisoned;
static constexpr unsigned COMP_CTX_ID=2,VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING=0x107;
static constexpr unsigned CMD_REQ_OFF=0,CMD_RESP_CAP=64;
alignas(8) static uint8_t commandBytes[128];
static uint8_t* g_cmdBuf=commandBytes;
alignas(4096) static uint8_t source[4096];
static std::vector<uint8_t> image;
static unsigned depth,operation,failAt=~0u;
static bool published=true,hostAttached,pendingRead,detachCompleted,detachBeforeFailedReply;
static bool lastResponseOk;
static uint64_t borrowed;
static std::vector<unsigned> commands;
struct GpuCmdGuard {GpuCmdGuard(){CHECK(!depth);++depth;}~GpuCmdGuard(){--depth;}};
static GpuTexture* CompTex(GpuTexId id){return id==7?&texture:nullptr;}
static bool outcome(unsigned kind){CHECK(depth==1);commands.push_back(kind);return operation++!=failAt;}
static bool ResourceAttachBackingVirt(uint32_t resource,uint64_t snapshot,uint32_t bytes,bool* submitted) {
    CHECK(resource==77 && snapshot==reinterpret_cast<uint64_t>(source) && bytes==48);
    *submitted=published;
    if(!published)return false;
    borrowed=snapshot;hostAttached=true;return outcome(1);
}
static bool TransferToHost3D(uint32_t ctx,uint32_t resource,uint32_t x,uint32_t y,
                              uint32_t w,uint32_t h,uint32_t stride,uint32_t height) {
    CHECK(ctx==2 && resource==77 && !x && !y && w==4 && h==3 && stride==4 && height==3 && hostAttached);
    pendingRead=true;return outcome(2);
}
static bool AppFenceLocked(uint32_t ctx) {
    CHECK(ctx==2 && pendingRead && hostAttached);
    if(!outcome(3))return false;
    const auto* data=reinterpret_cast<const uint8_t*>(borrowed);
    image.assign(data,data+48);pendingRead=false;return true;
}
static uint32_t SubmitCommand(uint32_t bytes,uint32_t response,bool fenced=false) {
    const auto* request=reinterpret_cast<const VirtioGpuCtxResource*>(g_cmdBuf);
    CHECK(bytes==sizeof(*request) && response==CMD_RESP_CAP &&
          request->hdr.type==VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING &&
          request->hdr.ctx_id==0 && request->resource_id==77 && !request->padding);
    CHECK(!pendingRead && hostAttached && fenced); // detach itself must be fenced
    lastResponseOk=outcome(4);
    if(lastResponseOk || detachBeforeFailedReply){hostAttached=false;detachCompleted=true;borrowed=0;}
    return lastResponseOk?24:0;
}
static bool CmdRespOk(uint32_t bytes){return bytes==24 && lastResponseOk;}
#include "../../drivers/virtio_gpu/resource_control.inc"
#include "../../drivers/virtio_gpu/owned_texture_upload.inc"
static void reset() {
    CHECK(!depth);texture={};g_compFrameAssembling=g_controlQueuePoisoned=false;
    operation=0;failAt=~0u;published=true;hostAttached=pendingRead=detachCompleted=false;
    detachBeforeFailedReply=false;
    borrowed=0;commands.clear();image.clear();std::memset(commandBytes,0,sizeof(commandBytes));
    for(unsigned i=0;i<sizeof(source);++i)source[i]=static_cast<uint8_t>(i*7+11);
}
static void exercise() {
    reset();published=false;
    CHECK(CompUploadOwnedTexture(7,reinterpret_cast<uint64_t>(source),48)==GpuCaptureResult::Rejected);
    CHECK(!texture.quarantined && !texture.uploadBackingAttached && commands.empty());
    for(unsigned failure=0;failure<4;++failure) {
        reset();failAt=failure;
        CHECK(CompUploadOwnedTexture(7,reinterpret_cast<uint64_t>(source),48)==GpuCaptureResult::Uncertain);
        CHECK(texture.quarantined && texture.uploadBackingAttached && hostAttached && !detachCompleted);
        CHECK(commands.size()==failure+1);
        const auto count=commands.size();
        CHECK(CompUploadOwnedTexture(7,reinterpret_cast<uint64_t>(source),48)==GpuCaptureResult::Rejected);
        CHECK(commands.size()==count); // uncertain source cannot be silently reused
    }
    // The device may detach successfully and then lose or invalidate its reply.
    // Device-side effects do not let the caller infer safe source retirement.
    reset();failAt=3;detachBeforeFailedReply=true;
    CHECK(CompUploadOwnedTexture(7,reinterpret_cast<uint64_t>(source),48)==GpuCaptureResult::Uncertain);
    CHECK(!hostAttached && detachCompleted && texture.quarantined && texture.uploadBackingAttached);
    const auto failedCount=commands.size();
    CHECK(CompUploadOwnedTexture(7,reinterpret_cast<uint64_t>(source),48)==GpuCaptureResult::Rejected);
    CHECK(commands.size()==failedCount);
    reset();
    // Repeated successful uploads permit immediate source reuse only after the
    // upload completion and the fenced detach both complete. Retained pixels
    // must not follow subsequent writes to the released source snapshot.
    for(unsigned round=0;round<16;++round) {
        for(unsigned i=0;i<48;++i)source[i]=static_cast<uint8_t>(round*31+i);
        std::vector<uint8_t> expected(source,source+48);
        CHECK(CompUploadOwnedTexture(7,reinterpret_cast<uint64_t>(source),48)==GpuCaptureResult::Complete);
        CHECK(!texture.quarantined && !texture.uploadBackingAttached && !hostAttached && !pendingRead && detachCompleted);
        CHECK(image==expected && !borrowed);
        std::memset(source,0x55,48);CHECK(image==expected);
    }
    CHECK(commands.size()==16*4); // no extra empty fenced submit after detach
    for(unsigned i=0;i<commands.size();++i)CHECK(commands[i]==i%4+1);
    reset();g_compFrameAssembling=true;
    CHECK(CompUploadOwnedTexture(7,reinterpret_cast<uint64_t>(source),48)==GpuCaptureResult::Rejected && commands.empty());
    reset();CHECK(CompUploadOwnedTexture(7,reinterpret_cast<uint64_t>(source)+1,48)==GpuCaptureResult::Rejected && commands.empty());
    reset();CHECK(CompUploadOwnedTexture(7,reinterpret_cast<uint64_t>(source),44)==GpuCaptureResult::Rejected && commands.empty());
}
}
int main(){exercise();std::puts("GPU_OWNED_UPLOAD_ACTUAL_BODY_PASS fenced detach, failures, retained source, immediate reuse");}
