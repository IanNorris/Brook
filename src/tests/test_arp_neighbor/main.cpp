// ARP neighbor table and pending-frame queue state machine (arp_neighbor.h).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../../kernel/src/arp_neighbor.h"
using namespace brook;

using Table=ArpNeighborTable<8,6,3,64>;
using R=Table::Result;
static const uint8_t macA[6]={2,0,0,0,0,0xa},macB[6]={2,0,0,0,0,0xb};

static void frame(uint8_t* f,uint8_t tag) { memset(f,0,64);f[12]=0x08;f[63]=tag; }

static void unresolvedLifecycle() {
    Table t;
    auto r=t.Resolve(1,1000,false);assert(r.result==R::Failed && !r.sendRequest && t.Count()==0);
    r=t.Resolve(1,1000,true);assert(r.result==R::Pending && r.sendRequest);
    r=t.Resolve(1,1100,true);assert(r.result==R::Pending && !r.sendRequest);   // shared probe timer
    r=t.Resolve(1,1500,true);assert(r.result==R::Pending && r.sendRequest);
    r=t.Resolve(1,2000,true);assert(r.result==R::Pending && r.sendRequest);
    r=t.Resolve(1,2499,true);assert(r.result==R::Pending && !r.sendRequest);
    r=t.Resolve(1,2500,true);assert(r.result==R::Failed && t.Count()==0);
    assert(t.Stats().requests==3 && t.Stats().failures==1);
    // A fresh attempt starts over.
    r=t.Resolve(1,2600,true);assert(r.result==R::Pending && r.sendRequest);
    // Confirmation resolves; Found copies the MAC.
    assert(t.Confirm(1,macA,2601,false));
    r=t.Resolve(1,2602,false);assert(r.result==R::Found && !memcmp(r.mac,macA,6) && t.Stats().resolved==1);
}

static void learningRules() {
    Table t;
    assert(!t.Confirm(5,macA,1,false) && t.Count()==0);  // unsolicited, absent, not for us
    assert(t.Confirm(5,macA,1,true) && t.Stats().created==1);
    assert(t.Confirm(5,macB,2,false));                     // gratuitous update of existing entry
    auto r=t.Resolve(5,3,false);assert(r.result==R::Found && !memcmp(r.mac,macB,6));
    assert(t.Stats().macChanges==1 && t.Stats().updated==1);
    // A late reply after the resolver failed still lands if addressed to us.
    for(uint64_t now=10;;now+=500){if(t.Resolve(6,now,true).result==R::Failed)break;}
    assert(!t.Has(6) && t.Confirm(6,macA,2000,true) && t.Resolve(6,2001,false).result==R::Found);
}

static void queueing() {
    Table t;uint8_t f[64],out[64];uint32_t len=0;void* ctx=nullptr;int nic=0;
    // Queue three frames, the per-neighbor limit; the fourth displaces the oldest.
    for(uint8_t n=1;n<=4;++n){frame(f,n);auto r=t.ResolveOrQueue(9,100,f,sizeof(f),&nic);
        assert(r.result==R::Pending && r.sendRequest==(n==1));}
    assert(t.QueuedFrames()==3 && t.Stats().queueDrops==1);
    assert(!t.PopReady(out,sizeof(out),&len,&ctx));          // unresolved: nothing released
    t.Confirm(9,macA,150,false);
    for(uint8_t want=2;want<=4;++want){
        assert(t.PopReady(out,sizeof(out),&len,&ctx) && len==64 && ctx==&nic);
        assert(out[63]==want && !memcmp(out,macA,6));        // FIFO, destination filled
    }
    assert(!t.PopReady(out,sizeof(out),&len,&ctx) && t.Stats().released==3);
    // Resolved neighbors are not queued.
    frame(f,9);auto r=t.ResolveOrQueue(9,160,f,sizeof(f),&nic);assert(r.result==R::Found && t.QueuedFrames()==0);
    // The pool (6) is shared: the globally oldest frame is displaced.
    for(uint32_t ip=20;ip<23;++ip)for(uint8_t n=0;n<2;++n){frame(f,uint8_t(ip*2+n));t.ResolveOrQueue(ip,200,f,sizeof(f),&nic);}
    assert(t.QueuedFrames()==6);
    frame(f,99);t.ResolveOrQueue(23,201,f,sizeof(f),&nic);
    assert(t.QueuedFrames()==6 && t.Stats().queueDrops==2);
    t.Confirm(20,macB,202,true);
    assert(t.PopReady(out,sizeof(out),&len,&ctx) && out[63]==41 && !t.PopReady(out,sizeof(out),&len,&ctx));
    // Failure drops queued frames; flush drops everything.
    const auto drops=t.Stats().failDrops;
    for(uint64_t now=203;now<=1800;now+=100){uint32_t ip=0;while(t.NextProbe(now,&ip)){}}
    assert(t.QueuedFrames()==0 && t.Stats().failDrops==drops+5 && !t.Has(21) && !t.Has(22) && !t.Has(23));
    frame(f,1);t.ResolveOrQueue(30,2000,f,sizeof(f),&nic);t.Flush();
    assert(t.Count()==0 && t.QueuedFrames()==0);
    // Invalid frames are refused, not stored.
    r=t.ResolveOrQueue(31,2100,f,65,&nic);assert(r.result==R::Pending && t.QueuedFrames()==0);
}

static void serviceProbes() {
    Table t;uint32_t ip=0;
    t.Resolve(1,0,true);t.Resolve(2,0,true);
    assert(!t.NextProbe(0,&ip));                              // both just sent their first request
    assert(t.NextProbe(500,&ip) && t.NextProbe(500,&ip) && !t.NextProbe(500,&ip));
    assert(t.NextProbe(1000,&ip) && t.NextProbe(1000,&ip) && !t.NextProbe(1000,&ip));
    assert(!t.NextProbe(1499,&ip) && t.Count()==2);
    assert(!t.NextProbe(1500,&ip) && t.Count()==0 && t.Stats().failures==2 && t.Stats().requests==6);
}

static void eviction() {
    Table t;uint8_t f[64];int nic=0;
    // Insertion always succeeds: the least recently used resolved entry goes.
    for(uint32_t ip=1;ip<=8;++ip)assert(t.Confirm(ip,macA,ip,true));
    assert(t.Resolve(1,20,false).result==R::Found);            // 1 is now the most recent
    assert(t.Confirm(100,macB,21,true) && t.Count()==8 && !t.Has(2) && t.Has(1) && t.Stats().evictions==1);
    // A miss also evicts rather than failing.
    frame(f,1);assert(t.ResolveOrQueue(101,22,f,sizeof(f),&nic).result==R::Pending && !t.Has(3));
    // Unresolved entries go only when no resolved entry remains, oldest first,
    // dropping their frames.
    Table u;
    for(uint32_t ip=1;ip<=8;++ip){frame(f,uint8_t(ip));
        if(ip<=6)u.ResolveOrQueue(ip,ip,f,sizeof(f),&nic);else u.Resolve(ip,ip,true);}
    assert(u.QueuedFrames()==6);
    const auto drops=u.Stats().failDrops;
    assert(u.Confirm(50,macA,9,true) && !u.Has(1) && u.Has(2) && u.Stats().failDrops==drops+1);
}

static void refresh() {
    ArpNeighborTiming timing;timing.refreshMs=1000;
    Table t(timing);
    t.Confirm(7,macA,0,true);
    auto r=t.Resolve(7,999,false);assert(r.result==R::Found && !r.sendRequest);
    r=t.Resolve(7,1000,false);assert(r.result==R::Found && r.sendRequest && t.Stats().refreshProbes==1);
    r=t.Resolve(7,1100,false);assert(r.result==R::Found && !r.sendRequest);
    r=t.Resolve(7,1500,false);assert(r.result==R::Found && r.sendRequest);
    t.Confirm(7,macB,1600,false);                               // answered: fresh again
    r=t.Resolve(7,1700,false);assert(r.result==R::Found && !r.sendRequest && !memcmp(r.mac,macB,6));
    // Unanswered refreshes keep using the old MAC, then resolve from scratch.
    r=t.Resolve(7,2600,false);assert(r.result==R::Found && r.sendRequest);
    r=t.Resolve(7,3100,false);assert(r.result==R::Found && r.sendRequest);
    r=t.Resolve(7,3600,false);assert(r.result==R::Found && r.sendRequest);
    r=t.Resolve(7,4099,false);assert(r.result==R::Found && !r.sendRequest);
    r=t.Resolve(7,4100,true);assert(r.result==R::Pending && r.sendRequest && t.Stats().refreshFailures==1);
    // An idle entry is still used on its next send, with a refresh probe.
    t.Confirm(8,macA,5000,true);
    r=t.Resolve(8,500000,false);assert(r.result==R::Found && r.sendRequest);
}

// The Dell incident shape: a LAN with many hosts, a resolver that issues its
// request from the receive path, and a reply processed only afterwards.
static void dellScenario() {
    using Big=ArpNeighborTable<64,16,4,1514>;
    static Big t;uint8_t synAck[60]{},out[1514];uint32_t len=0;void* ctx=nullptr;int nic=0;
    for(uint32_t n=0;n<300;++n)t.Confirm(1000+n,macA,n,true);   // hosts that ARPed for us
    assert(t.Count()==64);
    synAck[59]=0x12;
    auto r=t.ResolveOrQueue(8,400,synAck,sizeof(synAck),&nic);
    assert(r.result==Big::Result::Pending && r.sendRequest && t.QueuedFrames()==1);
    // Many more hosts appear before the reply is processed; the queued entry
    // survives because resolved entries are evicted first.
    for(uint32_t n=0;n<300;++n)t.Confirm(5000+n,macA,401,true);
    assert(t.Has(8));
    assert(t.Confirm(8,macB,402,true));                         // the host's reply
    assert(t.PopReady(out,sizeof(out),&len,&ctx) && len==60 && out[59]==0x12 && !memcmp(out,macB,6));
    // Repeated replies keep the entry current; no further requests are needed.
    for(int n=0;n<5;++n)t.Confirm(8,macB,403+n,false);
    r=t.Resolve(8,500,false);assert(r.result==Big::Result::Found && !r.sendRequest);
    // Flood: sends to 1000 unresolved destinations stay bounded and the live
    // neighbor remains usable throughout.
    for(uint32_t n=0;n<1000;++n){uint8_t f[60]{};t.ResolveOrQueue(20000+n,600,f,sizeof(f),&nic);}
    assert(t.QueuedFrames()<=16 && t.Count()==64);
    Big::State state;assert(t.Has(8,&state) && state==Big::State::Valid);  // recently used: kept
    unsigned unresolved=0;t.ForEach([&](const Big::EntryView& v){unresolved+=v.state==Big::State::Incomplete;});
    assert(unresolved==32);
    r=t.ResolveOrQueue(8,602,synAck,sizeof(synAck),&nic);assert(r.result==Big::Result::Found);
}

int main() {
    unresolvedLifecycle();
    learningRules();
    queueing();
    serviceProbes();
    eviction();
    refresh();
    dellScenario();
    std::puts("arp neighbor: PASS");
}
