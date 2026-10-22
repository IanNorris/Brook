// Actual Begin/Finish/synchronous and emergency-admission bodies. Fake device
// publication/completion and clocks arrange ownership races deterministically.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include "../../drivers/virtio_gpu/control_storage.h"
#include "../../drivers/virtio_gpu/preparation_sequence.h"
#include "../../drivers/virtio_gpu/retirement_protocol.h"
using namespace brook;
namespace {
constexpr uint32_t CMD_PAGES=16,CMD_REQ_OFF=0,CMD_RESP_OFF=15*4096,CMD_RESP_CAP=4096;
constexpr uint16_t VIRTQ_DESC_F_NEXT=1,VIRTQ_DESC_F_WRITE=2;
alignas(4096) uint8_t firstBytes[CMD_PAGES*4096],secondBytes[CMD_PAGES*4096];
uint8_t vertices[256];
GpuCommandStorage g_legacyCommand,second;
uint8_t* g_cmdBuf;
uint16_t g_queueSize=8,g_availIdxShadow,g_usedIdxShadow;
uint16_t usedIndex,availIndex,availRing[8];
volatile uint16_t* g_usedIdx=&usedIndex;
volatile uint16_t* g_availIdx=&availIndex;
uint16_t* g_availRing=availRing;
VirtqDesc descriptors[8];VirtqDesc* g_descTable=descriptors;
VirtqUsedElem usedRing[8];VirtqUsedElem* g_usedRing=usedRing;
unsigned notifications,warnings,clocks,failures,completeAtClock;
uint64_t now;
bool runtime=true,clockAvailable=true,completeOnNotify;
std::function<void()> notifyHook;
enum class FramePhase {QueueSetup,FenceWait,AckWait,VertexUpload,CommandBuild,DisplayFlush};
struct FrameTimingScope {explicit FrameTimingScope(FramePhase){} void Change(FramePhase){}};
static void FrameTimingCommand(uint32_t,bool,uint64_t){}
static void* ProcessCurrent(){return runtime?reinterpret_cast<void*>(1):nullptr;}
static bool KvmClockReadStableNs(uint64_t*);
static void NotifyControlQueue();
bool eventWait=false;
std::function<bool(uint16_t,uint64_t,uint64_t)> eventPauseHook;
static bool GpuCompletionEnabled(){return eventWait;}
static bool GpuCompletionPause(uint16_t used,uint64_t start,uint64_t deadline){return eventPauseHook?eventPauseHook(used,start,deadline):false;}
static void SerialPuts(const char*){++warnings;}
static void SerialPrintf(const char*,...){++warnings;}
#include "../../drivers/virtio_gpu/control_submit.inc"

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;}}while(0)
static void complete() {
    auto* p=g_pendingCommand;CHECK(p);
    if(!p)return;
    auto* reply=reinterpret_cast<VirtioGpuCtrlHdr*>(p->bytes+CMD_RESP_OFF);
    *reply={};reply->type=0x1100;reply->ctx_id=p->expectedContext;
    reply->flags=p->expectedFence?1:0;reply->fence_id=p->expectedFence;
    usedRing[p->usedPosition%g_queueSize]={p->head,sizeof(VirtioGpuCtrlHdr)};
    usedIndex=static_cast<uint16_t>(p->usedPosition+1);
}
static bool KvmClockReadStableNs(uint64_t* ns) {
    ++clocks;if(completeAtClock==clocks)complete();
    *ns=now;return clockAvailable;
}
static void NotifyControlQueue(){
    ++notifications;
    if(notifyHook)notifyHook();
    if(completeOnNotify)complete();
}
static void reset() {
    // Each case is a fresh fake device with no concurrent DMA producer.
    g_pendingCommand=nullptr;g_controlQueuePoisoned=false;g_nextRenderFence=0;
    g_legacyCommand.~GpuCommandStorage();new(&g_legacyCommand) GpuCommandStorage{};
    second.~GpuCommandStorage();new(&second) GpuCommandStorage{};
    std::memset(firstBytes,0,sizeof(firstBytes));std::memset(secondBytes,0,sizeof(secondBytes));
    std::memset(descriptors,0,sizeof(descriptors));std::memset(usedRing,0,sizeof(usedRing));
    std::memset(availRing,0,sizeof(availRing));std::memset(vertices,0x5a,sizeof(vertices));
    g_legacyCommand.bytes=g_cmdBuf=firstBytes;g_legacyCommand.physical=0x100000;
    second.bytes=secondBytes;second.physical=0x200000;
    g_availIdxShadow=g_usedIdxShadow=usedIndex=availIndex=0;
    notifications=warnings=clocks=completeAtClock=0;now=100;
    runtime=clockAvailable=true;completeOnNotify=false;notifyHook={};eventWait=false;eventPauseHook={};
    auto* hdr=reinterpret_cast<VirtioGpuCtrlHdr*>(firstBytes);hdr->type=0x205;hdr->ctx_id=2;
    auto* next=reinterpret_cast<VirtioGpuCtrlHdr*>(secondBytes);*next=*hdr;
}
constexpr GpuCompletionWaitLimits limits{1000,40,4};
static uint8_t* g_fbBacking=vertices;
static uint32_t g_fbW=640,g_fbH=480;
constexpr uint32_t RESOURCE_FB=1;
static unsigned emergencyCalls,frameFlushes;
static bool frameTestActive;
static bool SetScanout(uint32_t,uint32_t,uint32_t,uint32_t){++emergencyCalls;return true;}
static bool TransferToHost2D(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t){++emergencyCalls;return true;}
static bool ResourceFlush(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){++emergencyCalls;if(frameTestActive){++frameFlushes;CHECK(!g_pendingCommand);}return true;}
#include "../../drivers/virtio_gpu/panic_present.inc"

static void ownership() {
    reset();CHECK(ControlQueueEmergencySafe());
    auto token=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP,true);CHECK(token);
    CHECK(notifications==1 && availIndex==1 && g_usedIdxShadow==0);
    CHECK(g_legacyCommand.state==GpuCommandStorageState::Published);
    CHECK(descriptors[0].addr==0x100000 && descriptors[1].addr==0x10f000);
    CHECK(descriptors[0].len==128 && descriptors[1].len==4096);
    uint8_t request[128];std::memcpy(request,firstBytes,sizeof(request));
    VirtqDesc before[8];std::memcpy(before,descriptors,sizeof(before));
    // CPU prepares a genuinely independent future packet while the transfer
    // request/response, descriptor pair and vertex backing remain device-owned.
    std::memset(secondBytes+24,0xa7,1000);
    CHECK(!BeginCommand(second,128,CMD_RESP_CAP,true));
    CHECK(second.generation==0 && g_nextRenderFence==1);
    CHECK(!std::memcmp(request,firstBytes,sizeof(request)) && !std::memcmp(before,descriptors,sizeof(before)));
    for(auto v:vertices)CHECK(v==0x5a);
    CHECK(!FinishCommand({token.storage,token.generation+1},limits));
    CHECK(!FinishCommand({reinterpret_cast<GpuCommandStorage*>(1),1},limits));
    CHECK(g_usedIdxShadow==0 && g_pendingCommand==token.storage);
    completeAtClock=3;
    CHECK(FinishCommand(token,limits)==24 && g_usedIdxShadow==1 && !g_pendingCommand);
    CHECK(g_legacyCommand.state==GpuCommandStorageState::Completed && !g_controlQueuePoisoned);
    CHECK(!FinishCommand(token,limits) && g_usedIdxShadow==1);
    auto other=BeginCommand(second,1024,CMD_RESP_CAP,true);CHECK(other);
    CHECK(!FinishCommand(token,limits) && g_pendingCommand==&second);
    complete();CHECK(FinishCommand(other,limits)==24 && g_usedIdxShadow==2);
}

static void malformed() {
    for(unsigned mode=0;mode<7;++mode){
        reset();auto token=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP,true);complete();
        auto* response=reinterpret_cast<VirtioGpuCtrlHdr*>(firstBytes+CMD_RESP_OFF);
        switch(mode){
        case 0:usedRing[0].id=7;break;
        case 1:usedRing[0].len=23;break;
        case 2:usedRing[0].len=4097;break;
        case 3:++response->fence_id;break;
        case 4:++response->ctx_id;break;
        case 5:response->flags=0;break;
        case 6:usedIndex=2;break;
        }
        CHECK(!FinishCommand(token,limits));
        CHECK(g_controlQueuePoisoned && g_pendingCommand==&g_legacyCommand);
        CHECK(g_legacyCommand.state==GpuCommandStorageState::Quarantined && g_usedIdxShadow==0);
        CHECK(g_legacyCommand.bytes==firstBytes && g_cmdBuf!=firstBytes);
        uint8_t original[128];std::memcpy(original,firstBytes,128);
        std::memset(g_cmdBuf,0xcc,128);CHECK(!std::memcmp(original,firstBytes,128));
        CHECK(!BeginCommand(second,128,CMD_RESP_CAP) && !ControlQueueEmergencySafe());
    }
}

static void boundsAndTimeout() {
    const uint32_t bad[][2]={{23,4096},{61441,4096},{128,23},{128,4097}};
    for(const auto& pair:bad){
        reset();CHECK(!BeginCommand(g_legacyCommand,pair[0],pair[1]));
        CHECK(!notifications && !g_pendingCommand && g_legacyCommand.generation==0);
    }
    reset();completeOnNotify=true;
    CHECK(SubmitCommand(CMD_RESP_OFF,CMD_RESP_CAP,true)==24);
    CHECK(g_usedIdxShadow==1 && descriptors[0].len==61440);

    reset();auto token=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP);
    now+=1000;CHECK(!FinishCommand(token,limits));
    CHECK(clocks==2 && g_controlQueuePoisoned && g_usedIdxShadow==0);
    // A late device reply cannot make uncertain ownership reusable.
    complete();CHECK(!FinishCommand(token,limits));CHECK(!BeginCommand(second,128,CMD_RESP_CAP));
    reset();clockAvailable=false;token=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP);
    CHECK(!FinishCommand(token,limits) && clocks==1 && g_controlQueuePoisoned);
}

static void wrapAndEmergency() {
    reset();g_availIdxShadow=g_usedIdxShadow=availIndex=usedIndex=65535;
    auto token=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP,true);
    CHECK(token && token.storage->head==7 && descriptors[7].next==0 && availIndex==0);
    complete();CHECK(FinishCommand(token,limits)==24 && g_usedIdxShadow==0);
    reset();emergencyCalls=0;VgpuPanicPresent();CHECK(emergencyCalls==3);
    token=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP,true);
    emergencyCalls=0;VgpuPanicPresent();CHECK(!emergencyCalls && g_pendingCommand==token.storage);
    complete();VgpuPanicPresent();CHECK(!emergencyCalls); // available but unvalidated
    CHECK(FinishCommand(token,limits)==24);VgpuPanicPresent();CHECK(emergencyCalls==3);
    reset();notifyHook=[] {emergencyCalls=0;VgpuPanicPresent();CHECK(!emergencyCalls);};
    completeOnNotify=true;CHECK(SubmitCommand(128,CMD_RESP_CAP,true)==24);
    reset();usedIndex=1;emergencyCalls=0;VgpuPanicPresent();CHECK(!emergencyCalls);
    CHECK(!BeginCommand(g_legacyCommand,128,CMD_RESP_CAP) && g_controlQueuePoisoned);
}
} // namespace
static void preparationSequence() {
    for(unsigned mode=0;mode<5;++mode) {
        reset();bool prepared=false,finished=false;unsigned draws=0;
        const bool ready=GpuPrepareWhilePending(
            [&]{return BeginCommand(g_legacyCommand,128,CMD_RESP_CAP);},
            [&]{
                prepared=true;CHECK(g_pendingCommand==&g_legacyCommand);
                uint8_t request[128];std::memcpy(request,firstBytes,sizeof(request));
                VirtqDesc descriptorsBefore[8];std::memcpy(descriptorsBefore,descriptors,sizeof(descriptors));
                std::memset(secondBytes+24,0xa7,1000);
                CHECK(!std::memcmp(request,firstBytes,sizeof(request)));
                CHECK(!std::memcmp(descriptorsBefore,descriptors,sizeof(descriptors)));
                for(auto v:vertices)CHECK(v==0x5a);
                // Early complete, rejected packet, rejected transfer, timeout,
                // and normal delayed completion respectively.
                if(mode==0)complete();
                if(mode==3)now+=limits.timeoutNs;
                return mode!=1;
            },
            [&](GpuCommandToken token){
                finished=true;
                if(mode!=3 && mode!=0)complete();
                if(mode==2)reinterpret_cast<VirtioGpuCtrlHdr*>(firstBytes+CMD_RESP_OFF)->type=0x1200;
                const auto len=FinishCommand(token,limits);
                return len>=sizeof(VirtioGpuCtrlHdr) &&
                    reinterpret_cast<VirtioGpuCtrlHdr*>(firstBytes+CMD_RESP_OFF)->type==0x1100;
            });
        if(ready){++draws;CHECK(!g_pendingCommand);}
        CHECK(prepared && finished);
        CHECK(draws==((mode==0 || mode==4)?1u:0u));
        CHECK((g_pendingCommand!=nullptr)==(mode==3));
    }
    reset();g_controlQueuePoisoned=true;
    CHECK(!GpuPrepareWhilePending(
        [&]{return BeginCommand(g_legacyCommand,128,CMD_RESP_CAP);},
        [&]{CHECK(false);return true;},
        [&](GpuCommandToken){CHECK(false);return true;}));
}

// The real frame orchestration with instrumented packet construction and
// display primitive; actual packet construction has its own boundary suite.
GpuCommandStorage& g_composeCommand=second;
uint32_t g_compScanoutW=1920,g_compScanoutH=1080,g_drawQuadCount,g_blurBarrierCount;
constexpr uint32_t COMP_CTX_ID=2,COMP_DRAW_VBUF_RES=13,COMP_SCANOUT_RES=16;
struct DrawQuadRec {int32_t dx,dy,dx2,dy2;uint32_t u0,v0,u1,v1,opacity;};
DrawQuadRec g_drawQuads[1];uint32_t* g_drawVtxBuf=reinterpret_cast<uint32_t*>(vertices);
unsigned frameMode,buildCalls;
uint32_t NdcBits(int32_t p,int32_t){return static_cast<uint32_t>(p);}
GpuCommandToken BeginVertexTransfer(uint32_t,uint32_t,uint32_t) {
    return BeginCommand(g_legacyCommand,128,CMD_RESP_CAP);
}
uint32_t BuildComposePacket(GpuCommandStorage& storage,int32_t,int32_t) {
    ++buildCalls;CHECK(g_pendingCommand==&g_legacyCommand);
    CHECK(storage.bytes!=g_legacyCommand.bytes);
    auto* hdr=reinterpret_cast<VirtioGpuCtrlHdr*>(storage.bytes);*hdr={};hdr->ctx_id=2;hdr->type=0x207;
    if(frameMode==4)now+=5000000000ull;
    return frameMode==1?0:128;
}
bool CmdRespOk(const GpuCommandStorage& storage,uint32_t len) {
    return len>=sizeof(VirtioGpuCtrlHdr) && storage.state==GpuCommandStorageState::Completed &&
        reinterpret_cast<const VirtioGpuCtrlHdr*>(storage.bytes+CMD_RESP_OFF)->type==0x1100;
}
bool CmdRespOk(uint32_t len){return CmdRespOk(g_legacyCommand,len);}
#include "../../drivers/virtio_gpu/resource_control.inc"
void resourceControlFences() {
    // Execute the actual resource builder, publication and response validator.
    // A detach ACK is not enough: validate its own fence before completion.
    for(unsigned fault=0;fault<8;++fault) {
        reset();
        notifyHook=[&] {
            const auto* request=reinterpret_cast<const VirtioGpuCtxResource*>(g_legacyCommand.bytes);
            CHECK(request->hdr.type==0x107 && request->hdr.ctx_id==0 &&
                  request->resource_id==0xabc && !request->padding);
            CHECK(g_descTable[g_pendingCommand->head].len==sizeof(VirtioGpuCtxResource));
            if(fault==6){now+=6000000000ULL;return;}
            complete();auto* reply=reinterpret_cast<VirtioGpuCtrlHdr*>(g_legacyCommand.bytes+CMD_RESP_OFF);
            if(fault==1)++reply->fence_id;
            if(fault==2)reply->flags=0;
            if(fault==3)reply->ctx_id=2;
            if(fault==4)usedRing[0].len=23;
            if(fault==5)reply->type=0x1200;
        };
        const bool ok=fault==7?ResourceControlLocked(0x107,0,0xabc):
                              ResourceControlLocked(0x107,0,0xabc,true);
        CHECK(ok==(fault==0 || fault==7));
        CHECK(notifications==1);
        CHECK(g_nextRenderFence==(fault==7?0u:1u));
        CHECK(g_controlQueuePoisoned==((fault>=1 && fault<=4) || fault==6));
        if(g_controlQueuePoisoned) {
            CHECK(g_pendingCommand && g_pendingCommand->state==GpuCommandStorageState::Quarantined);
            const auto prior=notifications;
            CHECK(!ResourceControlLocked(0x107,0,0xabc,true) && notifications==prior);
        }
    }
}
#include "../../drivers/virtio_gpu/compose_frame.inc"
void frameFailures() {
    reset();g_drawQuadCount=1;
    const auto inFlight=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP);
    CHECK(inFlight && !g_controlQueuePoisoned);
    CompEndFrameDraw();
    for(auto value:vertices)CHECK(value==0x5a);
    CHECK(notifications==1 && g_pendingCommand==inFlight.storage);
    complete();CHECK(FinishCommand(inFlight,limits)==sizeof(VirtioGpuCtrlHdr));
    for(unsigned mode=0;mode<6;++mode){
        reset();frameMode=mode;frameTestActive=true;frameFlushes=buildCalls=0;
        g_drawQuadCount=g_blurBarrierCount=1;
        notifyHook=[&]{
            if(frameMode==4)return;
            if(frameMode==5 && notifications==2){now+=5000000000ull;return;}
            complete();
            if((frameMode==2 && notifications==1) || (frameMode==3 && notifications==2))
                reinterpret_cast<VirtioGpuCtrlHdr*>(g_pendingCommand->bytes+CMD_RESP_OFF)->type=0x1200;
        };
        CompEndFrameDraw();
        CHECK(buildCalls==1);CHECK(frameFlushes==(mode==0?1u:0u));
        CHECK(notifications==((mode==0 || mode==3 || mode==5)?2u:1u));
        CHECK(g_drawQuadCount==(mode==0?0u:1u));
        if(mode==4 || mode==5){
            CHECK(g_controlQueuePoisoned && g_pendingCommand);
            // The next frame's vertices differ, but the timed-out transfer
            // still owns their backing. Re-entry must not write them, rebuild
            // a packet, or publish any new descriptors. A late used entry by
            // itself also does not release quarantine.
            uint8_t heldVertices[sizeof(vertices)];
            uint8_t heldRequest[sizeof(firstBytes)];
            uint8_t heldCompose[sizeof(secondBytes)];
            VirtqDesc heldDescriptors[8];
            std::memcpy(heldVertices,vertices,sizeof(vertices));
            std::memcpy(heldRequest,firstBytes,sizeof(firstBytes));
            std::memcpy(heldCompose,secondBytes,sizeof(secondBytes));
            std::memcpy(heldDescriptors,descriptors,sizeof(descriptors));
            const unsigned published=notifications,built=buildCalls;
            g_drawQuads[0].dx+=17;g_drawQuads[0].dy+=31;
            for(unsigned attempt=0;attempt<2;++attempt) {
                if(attempt)complete();
                CompEndFrameDraw();
                CHECK(!std::memcmp(heldVertices,vertices,sizeof(vertices)));
                CHECK(!std::memcmp(heldRequest,firstBytes,CMD_RESP_OFF));
                CHECK(!std::memcmp(heldCompose,secondBytes,CMD_RESP_OFF));
                CHECK(!std::memcmp(heldDescriptors,descriptors,sizeof(descriptors)));
                CHECK(notifications==published && buildCalls==built);
                CHECK(g_pendingCommand && g_controlQueuePoisoned);
            }
        }
        else {
            CHECK(!g_pendingCommand && !g_controlQueuePoisoned);
            // Retained storage/generations remain usable after a failed build,
            // valid transfer error or valid compose error, without device reset.
            frameMode=0;g_drawQuadCount=g_blurBarrierCount=1;
            const auto prior=frameFlushes;CompEndFrameDraw();CHECK(frameFlushes==prior+1);
        }
        frameTestActive=false;
    }
}

static void notificationOwnership() {
    reset();eventWait=true;
    auto token=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP,true);
    eventPauseHook=[&](uint16_t used,uint64_t start,uint64_t deadline) {
        CHECK(used==token.storage->usedPosition && start==100 && deadline==1100);
        CHECK(token.storage->state==GpuCommandStorageState::Published && g_pendingCommand==token.storage);
        CHECK(!BeginCommand(second,128,CMD_RESP_CAP));
        complete();return true;
    };
    CHECK(FinishCommand(token,limits)==sizeof(VirtioGpuCtrlHdr));
    CHECK(!g_pendingCommand && token.storage->state==GpuCommandStorageState::Completed);
    reset();eventWait=true;token=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP,true);
    eventPauseHook=[&](uint16_t,uint64_t start,uint64_t deadline) {
        CHECK(start==100&&deadline==1100);now=deadline;return true;
    };
    CHECK(!FinishCommand(token,limits) && g_controlQueuePoisoned);
    CHECK(token.storage->state==GpuCommandStorageState::Quarantined && g_pendingCommand==token.storage);
    reset();eventWait=true;token=BeginCommand(g_legacyCommand,128,CMD_RESP_CAP,true);
    eventPauseHook=[&](uint16_t,uint64_t,uint64_t) {
        complete();reinterpret_cast<VirtioGpuCtrlHdr*>(g_pendingCommand->bytes+CMD_RESP_OFF)->fence_id++;
        return true;
    };
    CHECK(!FinishCommand(token,limits)&&g_controlQueuePoisoned);
}

namespace reset_device {
uint8_t config;
volatile uint8_t* g_commonCfg=&config;
constexpr unsigned VIRTIO_COMMON_STATUS=20;
unsigned reads,writes,acknowledgeAfter;
static void mmio_write8(volatile uint8_t* address,unsigned offset,uint8_t value) {
    CHECK(address==g_commonCfg && offset==VIRTIO_COMMON_STATUS && value==0);
    ++writes;
}
static uint8_t mmio_read8(volatile uint8_t* address,unsigned offset) {
    CHECK(address==g_commonCfg && offset==VIRTIO_COMMON_STATUS && writes==1);
    now+=100;
    return ++reads>=acknowledgeAfter?0:0x0f;
}
#include "../../drivers/virtio_gpu/device_reset.inc"
static void exercise() {
    // An asynchronous reset retains old device state until acknowledgement.
    // The actual driver body must read status, including for immediate reset.
    for(unsigned delay:{1u,4u,100u}) {
        reset();runtime=false;reads=writes=0;acknowledgeAfter=delay;
        const bool stopped=ResetGpuDevice(limits);
        CHECK(stopped==(delay<=40) && writes==1 && reads==(delay<=40?delay:40));
        CHECK(clocks==0); // early boot cannot depend on scheduler/clock startup
    }
    reset();reads=writes=0;acknowledgeAfter=100;
    CHECK(!ResetGpuDevice(limits));
    CHECK(writes==1 && reads<40 && clocks>1); // elapsed runtime bound
}
}

int main(){
    resourceControlFences();
    reset_device::exercise();
    notificationOwnership();
    frameFailures();
    preparationSequence();
    ownership();malformed();boundsAndTimeout();wrapAndEmergency();
    std::printf("GPU_CONTROL_ACTUAL_BODY %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
