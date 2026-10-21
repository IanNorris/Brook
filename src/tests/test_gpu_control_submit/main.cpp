// Actual Begin/Finish/synchronous and emergency-admission bodies. Fake device
// publication/completion and clocks arrange ownership races deterministically.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include "../../drivers/virtio_gpu/control_storage.h"
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
enum class FramePhase {QueueSetup,FenceWait,AckWait};
struct FrameTimingScope {explicit FrameTimingScope(FramePhase){}};
static void FrameTimingCommand(uint32_t,bool,uint64_t){}
static void* ProcessCurrent(){return runtime?reinterpret_cast<void*>(1):nullptr;}
static bool KvmClockReadStableNs(uint64_t*);
static void NotifyControlQueue();
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
    runtime=clockAvailable=true;completeOnNotify=false;notifyHook={};
    auto* hdr=reinterpret_cast<VirtioGpuCtrlHdr*>(firstBytes);hdr->type=0x205;hdr->ctx_id=2;
    auto* next=reinterpret_cast<VirtioGpuCtrlHdr*>(secondBytes);*next=*hdr;
}
constexpr GpuCompletionWaitLimits limits{1000,40,4};
static uint8_t* g_fbBacking=vertices;
static uint32_t g_fbW=640,g_fbH=480;
constexpr uint32_t RESOURCE_FB=1;
static unsigned emergencyCalls;
static bool SetScanout(uint32_t,uint32_t,uint32_t,uint32_t){++emergencyCalls;return true;}
static bool TransferToHost2D(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint64_t){++emergencyCalls;return true;}
static bool ResourceFlush(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){++emergencyCalls;return true;}
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
int main(){
    ownership();malformed();boundsAndTimeout();wrapAndEmergency();
    std::printf("GPU_CONTROL_ACTUAL_BODY %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
