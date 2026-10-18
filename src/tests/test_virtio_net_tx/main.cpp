#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include "../../drivers/virtio_net/virtio_net_mod.cpp"

namespace {
int failures=0;
std::atomic<unsigned> notifications{0},badFrames{0};
std::atomic<bool> pauseFirst{false},entered{false},releaseFirst{false};
enum Mode { Normal, NoCompletion, BadId, BadAdvance };
Mode mode=Normal;
thread_local unsigned expectedId=1;
uint8_t dma[RX_BUF_SIZE];
uint16_t avail=0;volatile uint16_t used=0;
VirtqDesc descriptors[MAX_QUEUE_SIZE];
uint16_t available[MAX_QUEUE_SIZE];
VirtqUsedElem completions[MAX_QUEUE_SIZE];
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"NET_TX FAIL line %d: %s\n",__LINE__,#x);++failures;}}while(0)
void initialize() {
    g_txLock={};g_txFaulted=false;g_txPending=false;g_txq={};
    g_txBuf=dma;g_txBufPhys=0x1000;avail=0;used=0;
    g_txq.size=MAX_QUEUE_SIZE;g_txq.descTable=descriptors;
    g_txq.availIdx=&avail;g_txq.availRing=available;
    g_txq.usedIdx=&used;g_txq.usedRing=completions;
    memset(dma,0,sizeof(dma));memset(completions,0,sizeof(completions));
    notifications=0;badFrames=0;pauseFirst=false;entered=false;releaseFirst=false;mode=Normal;
}
void send(unsigned id) {
    expectedId=id;uint8_t payload[256];memset(payload,id,sizeof(payload));
    if(VirtioNetTransmit(nullptr,payload,sizeof(payload))!=0)++badFrames;
}
}
namespace brook {
extern "C" void SerialPuts(const char*) {}
extern "C" void SerialPrintf(const char*,...) {}
}
void VirtioNetTestNotify(uint16_t queue) {
    if(queue!=1){++badFrames;return;}
    unsigned count=++notifications;
    if(pauseFirst && count==1){entered=true;while(!releaseFirst)std::this_thread::yield();}
    std::this_thread::yield(); // Device can read after another producer is scheduled.
    const auto& descriptor=g_txq.descTable[0];
    if(descriptor.addr!=0x1000 || descriptor.len!=VIRTIO_NET_HDR_SIZE+256 ||
       descriptor.flags || descriptor.next)++badFrames;
    for(unsigned i=0;i<VIRTIO_NET_HDR_SIZE;i++)if(dma[i])++badFrames;
    for(unsigned i=0;i<256;i++)if(dma[VIRTIO_NET_HDR_SIZE+i]!=expectedId)++badFrames;
    if(mode==NoCompletion)return;
    auto& completion=g_txq.usedRing[g_txq.usedIdxShadow&(g_txq.size-1)];
    completion.id=mode==BadId?1:0;completion.len=0;
    *g_txq.usedIdx=static_cast<uint16_t>(g_txq.usedIdxShadow+(mode==BadAdvance?2:1));
}
int main() {
    initialize();pauseFirst=true;
    std::thread first([]{send(0x31);});
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!entered && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    if(!entered){releaseFirst=true;first.join();CHECK(false);return 1;}
    std::atomic<bool> secondStarted{false};
    std::thread second([&]{secondStarted=true;send(0x72);});
    while(!secondStarted)std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool exclusive=notifications==1;
    releaseFirst=true;first.join();second.join();
    CHECK(exclusive);CHECK(badFrames==0);CHECK(used==2 && avail==2);

    initialize();std::vector<std::thread> producers;
    for(unsigned i=1;i<=8;i++)producers.emplace_back([i]{for(unsigned n=0;n<64;n++)send(i);});
    for(auto& thread:producers)thread.join();
    CHECK(notifications==512 && badFrames==0 && used==512 && avail==512);

    initialize();g_txq.usedIdxShadow=65534;used=65534;
    g_txq.availIdxShadow=65534;avail=65534;
    for(unsigned i=0;i<4;i++)send(1);
    CHECK(used==2 && avail==2 && badFrames==0);
    unsigned before=notifications;
    CHECK(VirtioNetTransmit(nullptr,dma,UINT32_MAX)==-1);
    CHECK(VirtioNetTransmit(nullptr,nullptr,1)==-1);CHECK(notifications==before);

    // A slow completion must preserve DMA ownership without permanently
    // disabling the interface. Repeated sends cannot touch the pending frame.
    initialize();mode=NoCompletion;
    uint8_t pendingPayload[256];memset(pendingPayload,1,sizeof(pendingPayload));expectedId=1;
    CHECK(VirtioNetTransmit(nullptr,pendingPayload,sizeof(pendingPayload))==-11);
    CHECK(g_txPending && !g_txFaulted);
    uint8_t pendingSnapshot[RX_BUF_SIZE];memcpy(pendingSnapshot,dma,sizeof(dma));
    memset(pendingPayload,2,sizeof(pendingPayload));
    for(unsigned i=0;i<8;i++)
        CHECK(VirtioNetTransmit(nullptr,pendingPayload,sizeof(pendingPayload))==-11);
    CHECK(notifications==1 && avail==1 && !memcmp(pendingSnapshot,dma,sizeof(dma)));
    completions[0].id=0;used=1;mode=Normal;expectedId=2;
    CHECK(VirtioNetTransmit(nullptr,pendingPayload,sizeof(pendingPayload))==0);
    CHECK(!g_txPending && !g_txFaulted && notifications==2 && used==2 && avail==2 && badFrames==0);

    // The same late-completion path must reject corrupt device responses.
    for(Mode failure:{BadId,BadAdvance}) {
        initialize();mode=NoCompletion;expectedId=1;
        memset(pendingPayload,1,sizeof(pendingPayload));
        CHECK(VirtioNetTransmit(nullptr,pendingPayload,sizeof(pendingPayload))==-11);
        completions[0].id=failure==BadId?1:0;used=failure==BadAdvance?2:1;
        CHECK(VirtioNetTransmit(nullptr,pendingPayload,sizeof(pendingPayload))==-1);
        CHECK(g_txFaulted && notifications==1);
    }

    for(Mode failure:{BadId,BadAdvance}) {
        initialize();mode=failure;
        uint8_t payload[256];memset(payload,1,sizeof(payload));expectedId=1;
        CHECK(VirtioNetTransmit(nullptr,payload,sizeof(payload))==-1);CHECK(g_txFaulted);
        uint8_t snapshot[RX_BUF_SIZE];memcpy(snapshot,dma,sizeof(dma));
        memset(payload,2,sizeof(payload));
        CHECK(VirtioNetTransmit(nullptr,payload,sizeof(payload))==-1);
        CHECK(notifications==1 && avail==1 && !memcmp(snapshot,dma,sizeof(dma)));
    }
    std::printf("VIRTIO_NET_TX %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
