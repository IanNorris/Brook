#pragma once
#include <stdint.h>

namespace brook {

// IPv4 neighbor (ARP) table with a bounded queue of frames awaiting
// resolution. Pure state machine: the caller serializes every call (one
// lock), supplies monotonic millisecond ticks, transmits ARP requests when a
// call reports sendRequest, and transmits frames returned by PopReady. No
// call blocks, allocates or touches hardware, so the table can be driven from
// the receive path; a reply processed there completes resolution for frames
// queued by that same path.
//
// Semantics follow Linux arp_process()/neighbour.c in reduced form:
//  - Insertion always succeeds. A full table evicts the least recently used
//    resolved entry, or the oldest unresolved one (dropping its frames) when
//    unresolved entries fill half the table or no resolved entry exists.
//  - Received ARP updates an existing entry (Incomplete or Valid) whatever its
//    opcode, including unsolicited/gratuitous replies. The caller decides when
//    an absent entry may be created (a request or reply addressed to us).
//  - Resolved entries do not expire by age. Use after refreshMs since the last
//    confirmation sends up to maxProbes refresh requests while the old MAC
//    stays usable; if none is answered the entry reverts to unresolved.
//  - An unresolved entry sends maxProbes requests retryMs apart and then fails,
//    dropping its queued frames.
struct ArpNeighborTiming {
    uint64_t retryMs=500;
    uint32_t maxProbes=3;
    uint64_t refreshMs=60000;
};

struct ArpNeighborStats {
    uint64_t requests=0;       // requests for unresolved entries
    uint64_t refreshProbes=0;  // requests for resolved but unconfirmed entries
    uint64_t resolved=0;       // Incomplete -> Valid
    uint64_t created=0;        // entries created directly from received ARP
    uint64_t updated=0;        // confirmations of existing entries
    uint64_t macChanges=0;
    uint64_t failures=0;       // unresolved entries that exhausted their probes
    uint64_t refreshFailures=0;
    uint64_t evictions=0;
    uint64_t queued=0;
    uint64_t queueDrops=0;     // frames displaced by per-neighbor or pool limits
    uint64_t failDrops=0;      // frames dropped by failure, eviction or flush
    uint64_t released=0;       // queued frames returned for transmission
};

template<unsigned Capacity,unsigned QueueSlots,unsigned PerNeighbor,unsigned FrameMax>
class ArpNeighborTable {
    static_assert(Capacity>0 && QueueSlots>0 && PerNeighbor>0 && FrameMax>=6,"invalid ARP table shape");
    uint64_t RetryMs() const { return timing_.retryMs ? timing_.retryMs : ArpNeighborTiming{}.retryMs; }
    uint32_t MaxProbes() const { return timing_.maxProbes ? timing_.maxProbes : ArpNeighborTiming{}.maxProbes; }
    uint64_t RefreshMs() const { return timing_.refreshMs ? timing_.refreshMs : ArpNeighborTiming{}.refreshMs; }
public:
    enum class State : uint8_t { Free, Incomplete, Valid };
    enum class Result : uint8_t { Found, Pending, Failed };
    struct Lookup { Result result=Result::Pending; bool sendRequest=false; uint8_t mac[6]{}; };
    struct EntryView {
        uint32_t ip; State state; uint8_t mac[6]; uint32_t probes;
        uint64_t confirmedMs, usedMs; uint32_t queued;
    };

    // constexpr: kernel globals must be constant-initialized (no .init_array).
    // A zero timing field means its ArpNeighborTiming default, so the default
    // table is all-zero and the kernel global stays in .bss.
    constexpr ArpNeighborTable() : timing_{0,0,0} {}
    constexpr explicit ArpNeighborTable(ArpNeighborTiming timing) : timing_(timing) {}

    // Look up ip. A Valid entry returns Found (and may ask for a refresh
    // probe). Otherwise, when create is true, an Incomplete entry is created
    // or advanced; Failed is returned once its probes are exhausted.
    Lookup Resolve(uint32_t ip,uint64_t now,bool create) {
        Lookup out{};
        Entry* e=Find(ip);
        if(e && e->state==State::Valid){
            e->used=now;
            if(now-e->confirmed<RefreshMs()){Copy(out.mac,e->mac);out.result=Result::Found;return out;}
            if(e->probes>=MaxProbes() && now>=e->nextProbe){
                // Refresh requests went unanswered: the neighbor is gone or
                // changed address. Resolve again from scratch.
                ++stats_.refreshFailures;Restart(*e,now);
            } else {
                Copy(out.mac,e->mac);out.result=Result::Found;
                if(now>=e->nextProbe && e->probes<MaxProbes()){
                    ++e->probes;e->nextProbe=now+RetryMs();out.sendRequest=true;++stats_.refreshProbes;
                }
                return out;
            }
        }
        if(!e){
            if(!create){out.result=Result::Failed;return out;}
            e=Allocate(ip,now);Restart(*e,now);
        }
        if(e->probes>=MaxProbes() && now>=e->nextProbe){
            ++stats_.failures;Release(*e);out.result=Result::Failed;return out;
        }
        if(now>=e->nextProbe){++e->probes;e->nextProbe=now+RetryMs();out.sendRequest=true;++stats_.requests;}
        out.result=Result::Pending;return out;
    }

    // Resolve, retaining a copy of frame when ip is unresolved. A previously
    // failed neighbor starts a new resolution. Returns Found without queuing;
    // otherwise Pending, with the frame queued unless it is invalid.
    Lookup ResolveOrQueue(uint32_t ip,uint64_t now,const void* frame,uint32_t length,void* context) {
        Lookup out=Resolve(ip,now,true);
        if(out.result==Result::Failed)out=Resolve(ip,now,true);
        if(out.result!=Result::Pending)return out;
        if(!frame || length<6 || length>FrameMax){++stats_.queueDrops;return out;}
        unsigned mine=0;Slot* oldestMine=nullptr;Slot* oldest=nullptr;Slot* free=nullptr;
        for(auto& s:slots_){
            if(!s.used){if(!free)free=&s;continue;}
            if(!oldest || Before(s,*oldest))oldest=&s;
            if(s.ip==ip){++mine;if(!oldestMine || Before(s,*oldestMine))oldestMine=&s;}
        }
        Slot* target=free;
        if(mine>=PerNeighbor)target=oldestMine;
        else if(!target)target=oldest;
        if(target->used)++stats_.queueDrops;
        target->used=true;target->ip=ip;target->context=context;target->length=length;target->sequence=++sequence_;
        const auto* bytes=static_cast<const uint8_t*>(frame);
        for(uint32_t i=0;i<length;++i)target->frame[i]=bytes[i];
        ++stats_.queued;return out;
    }

    // Record a received mapping. Returns false only when the entry is absent
    // and create is false. Queued frames for ip become available to PopReady.
    bool Confirm(uint32_t ip,const uint8_t mac[6],uint64_t now,bool create) {
        Entry* e=Find(ip);
        if(!e){
            if(!create)return false;
            e=Allocate(ip,now);e->used=now;++stats_.created;
        } else if(e->state==State::Incomplete)++stats_.resolved;
        else {
            ++stats_.updated;
            if(!Same(e->mac,mac))++stats_.macChanges;
        }
        e->state=State::Valid;Copy(e->mac,mac);e->confirmed=now;e->probes=0;e->nextProbe=0;
        return true;
    }

    // Remove the oldest queued frame whose neighbor is resolved, writing the
    // neighbor's MAC into the frame's Ethernet destination.
    bool PopReady(void* out,uint32_t capacity,uint32_t* length,void** context) {
        Slot* best=nullptr;const Entry* bestEntry=nullptr;
        for(auto& s:slots_){
            if(!s.used || (best && !Before(s,*best)))continue;
            const Entry* e=Find(s.ip);
            if(e && e->state==State::Valid){best=&s;bestEntry=e;}
        }
        if(!best || !out || best->length>capacity)return false;
        auto* bytes=static_cast<uint8_t*>(out);
        for(uint32_t i=0;i<best->length;++i)bytes[i]=best->frame[i];
        Copy(bytes,bestEntry->mac);
        if(length)*length=best->length;
        if(context)*context=best->context;
        best->used=false;++stats_.released;return true;
    }

    // Periodic service. Fails exhausted unresolved entries and reports the
    // next unresolved entry due for another request.
    bool NextProbe(uint64_t now,uint32_t* ip) {
        for(unsigned n=0;n<Capacity;++n){
            auto& e=entries_[(cursor_+n)%Capacity];
            if(e.state!=State::Incomplete)continue;
            if(e.probes>=MaxProbes()){
                if(now>=e.nextProbe){++stats_.failures;Release(e);}
                continue;
            }
            if(now<e.nextProbe)continue;
            ++e.probes;e.nextProbe=now+RetryMs();++stats_.requests;
            cursor_=(cursor_+n+1)%Capacity;if(ip)*ip=e.ip;return true;
        }
        return false;
    }

    void Flush() { for(auto& e:entries_)if(e.state!=State::Free)Release(e); }

    bool Has(uint32_t ip,State* state=nullptr) const {
        const Entry* e=Find(ip);if(state)*state=e?e->state:State::Free;return e!=nullptr;
    }
    unsigned Count() const { unsigned n=0;for(const auto& e:entries_)n+=e.state!=State::Free;return n; }
    unsigned QueuedFrames() const { unsigned n=0;for(const auto& s:slots_)n+=s.used;return n; }
    template<class F> void ForEach(F f) const {
        for(const auto& e:entries_){
            if(e.state==State::Free)continue;
            EntryView v{e.ip,e.state,{},e.probes,e.confirmed,e.used,0};Copy(v.mac,e.mac);
            for(const auto& s:slots_)v.queued+=s.used && s.ip==e.ip;
            f(v);
        }
    }
    const ArpNeighborStats& Stats() const { return stats_; }
    static constexpr unsigned kCapacity=Capacity;

private:
    struct Entry {
        State state=State::Free;uint32_t ip=0;uint8_t mac[6]{};uint32_t probes=0;
        uint64_t confirmed=0,used=0,nextProbe=0,started=0;
    };
    struct Slot {
        bool used=false;uint32_t ip=0;void* context=nullptr;uint32_t length=0;uint64_t sequence=0;
        uint8_t frame[FrameMax]{};
    };
    static void Copy(uint8_t* d,const uint8_t* s) { for(unsigned i=0;i<6;++i)d[i]=s[i]; }
    static bool Same(const uint8_t* a,const uint8_t* b) { for(unsigned i=0;i<6;++i)if(a[i]!=b[i])return false;return true; }
    static bool Before(const Slot& a,const Slot& b) { return a.sequence<b.sequence; }
    Entry* Find(uint32_t ip) { for(auto& e:entries_)if(e.state!=State::Free && e.ip==ip)return &e;return nullptr; }
    const Entry* Find(uint32_t ip) const { for(const auto& e:entries_)if(e.state!=State::Free && e.ip==ip)return &e;return nullptr; }
    void Restart(Entry& e,uint64_t now) { e.state=State::Incomplete;e.probes=0;e.nextProbe=now;e.started=now; }
    void DropFrames(uint32_t ip) { for(auto& s:slots_)if(s.used && s.ip==ip){s.used=false;++stats_.failDrops;} }
    void Release(Entry& e) { DropFrames(e.ip);e=Entry{}; }
    static uint64_t Activity(const Entry& e) { return e.used>e.confirmed?e.used:e.confirmed; }
    // Unresolved entries may hold at most half the table once it is full, so
    // a burst of sends to dead or new destinations replaces itself instead of
    // evicting every recently used resolved neighbor (gateway, bridge host).
    Entry* Allocate(uint32_t ip,uint64_t now) {
        Entry* victim=nullptr;Entry* oldestValid=nullptr;Entry* oldestIncomplete=nullptr;unsigned incomplete=0;
        for(auto& e:entries_){
            if(e.state==State::Free){victim=&e;break;}
            if(e.state==State::Valid){if(!oldestValid || Activity(e)<Activity(*oldestValid))oldestValid=&e;}
            else {++incomplete;if(!oldestIncomplete || e.started<oldestIncomplete->started)oldestIncomplete=&e;}
        }
        if(!victim)victim=oldestIncomplete && (incomplete>=Capacity/2 || !oldestValid)?oldestIncomplete:oldestValid;
        if(victim->state!=State::Free){++stats_.evictions;Release(*victim);}
        victim->ip=ip;victim->used=now;victim->confirmed=0;victim->started=now;
        return victim;
    }

    ArpNeighborTiming timing_;
    Entry entries_[Capacity]{};
    Slot slots_[QueueSlots]{};
    uint64_t sequence_=0;
    unsigned cursor_=0;
    ArpNeighborStats stats_{};
};

}
