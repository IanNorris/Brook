#pragma once
#include <stdint.h>

namespace brook {
// Scheduler-lock statistics (default off). Hold, wait and handoff durations
// are TSC cycles; steal is the waiting/holding vCPU's own reported KVM steal.
// One writer per CPU (the CPU itself, with IF clear while it waits for or holds
// a scheduler lock); readers use relaxed atomics and may see a row mid-update.
// Reset bumps the epoch; each writer clears its own row on first use in the new
// epoch, so no reader or remote CPU ever writes another CPU's row.
constexpr unsigned LockStatLocks=3;      // ready, all-process, pid
constexpr unsigned LockStatBuckets=20;   // bucket b: [2^(b+6), 2^(b+7)) cycles; 0 also < 64; last open
constexpr unsigned LockStatSiteSlots=48;
constexpr unsigned LockStatCpus=64;
constexpr uint16_t LockStatNoSite=0xffff;

inline unsigned LockStatBucket(uint64_t cycles){
    if(cycles<128)return 0;
    const unsigned b=unsigned(63-__builtin_clzll(cycles))-6;
    return b<LockStatBuckets?b:LockStatBuckets-1;
}

struct LockStatLockRow {
    uint64_t acquisitions=0,contended=0,waitCycles=0,holdCycles=0,maxWaitCycles=0,maxHoldCycles=0;
    uint64_t waitStealNs=0,holdStealNs=0,stolenHolds=0,handoffs=0,handoffCycles=0,maxHandoffCycles=0;
    uint64_t waitHist[LockStatBuckets]{},holdHist[LockStatBuckets]{},handoffHist[LockStatBuckets]{};
};
struct LockStatSite {
    const char* caller=nullptr;uint32_t line=0,lock=0;
    uint64_t acquisitions=0,contended=0,waitCycles=0,holdCycles=0,maxWaitCycles=0,maxHoldCycles=0,holdStealNs=0;
};
// Per-hold state lives in the lock itself: only the owner writes it.
struct LockStatHold {
    uint64_t acquiredTsc=0,acquiredSteal=0;
    uint16_t site=LockStatNoSite;uint8_t cpu=0;bool valid=false;
};

class SchedLockStats {
    struct alignas(64) Cpu {
        uint64_t epoch=0,siteOverflow=0;
        LockStatLockRow lock[LockStatLocks]{};
        LockStatSite site[LockStatSiteSlots]{};
    };
    uint32_t enabled_=0;
    uint64_t epoch_=1;
    Cpu cpu_[LockStatCpus]{};
    static uint64_t Load(const uint64_t& v){return __atomic_load_n(&v,__ATOMIC_RELAXED);}
    static void Store(uint64_t& v,uint64_t x){__atomic_store_n(&v,x,__ATOMIC_RELAXED);}
    static void Add(uint64_t& v,uint64_t d){Store(v,Load(v)+d);}
    static void Max(uint64_t& v,uint64_t x){if(x>Load(v))Store(v,x);}
    static void ClearRow(Cpu& c){
        Store(c.siteOverflow,0);
        auto* words=reinterpret_cast<uint64_t*>(&c.lock[0]);
        for(unsigned i=0;i<sizeof(c.lock)/8;++i)Store(words[i],0);
        for(auto& s:c.site){
            __atomic_store_n(&s.caller,static_cast<const char*>(nullptr),__ATOMIC_RELAXED);
            __atomic_store_n(&s.line,0u,__ATOMIC_RELAXED);__atomic_store_n(&s.lock,0u,__ATOMIC_RELAXED);
            Store(s.acquisitions,0);Store(s.contended,0);Store(s.waitCycles,0);Store(s.holdCycles,0);
            Store(s.maxWaitCycles,0);Store(s.maxHoldCycles,0);Store(s.holdStealNs,0);
        }
    }
    Cpu* Row(unsigned cpu){
        if(cpu>=LockStatCpus)return nullptr;
        auto& c=cpu_[cpu];const auto epoch=__atomic_load_n(&epoch_,__ATOMIC_ACQUIRE);
        if(Load(c.epoch)!=epoch){ClearRow(c);__atomic_store_n(&c.epoch,epoch,__ATOMIC_RELEASE);}
        return &c;
    }
    static uint16_t FindSite(Cpu& c,const char* caller,uint32_t line,unsigned lock){
        const uint64_t key=(reinterpret_cast<uintptr_t>(caller)>>3)^(uint64_t(line)*0x9e3779b97f4a7c15ull)^lock;
        unsigned slot=unsigned((key^(key>>29))%LockStatSiteSlots);
        for(unsigned n=0;n<LockStatSiteSlots;++n,slot=(slot+1)%LockStatSiteSlots){
            auto& s=c.site[slot];const char* existing=__atomic_load_n(&s.caller,__ATOMIC_RELAXED);
            if(!existing){
                __atomic_store_n(&s.line,line,__ATOMIC_RELAXED);__atomic_store_n(&s.lock,uint32_t(lock),__ATOMIC_RELAXED);
                __atomic_store_n(&s.caller,caller,__ATOMIC_RELEASE);return uint16_t(slot);
            }
            if(existing==caller && __atomic_load_n(&s.line,__ATOMIC_RELAXED)==line &&
               __atomic_load_n(&s.lock,__ATOMIC_RELAXED)==lock)return uint16_t(slot);
        }
        return LockStatNoSite;
    }
public:
    bool Enabled()const{return __atomic_load_n(&enabled_,__ATOMIC_RELAXED)!=0;}
    uint64_t Epoch()const{return __atomic_load_n(&epoch_,__ATOMIC_ACQUIRE);}
    // mode: -1 unchanged, 0 off, 1 on. Reset starts a new epoch.
    void Control(int mode,bool reset){
        if(reset)__atomic_fetch_add(&epoch_,1,__ATOMIC_ACQ_REL);
        if(mode>=0)__atomic_store_n(&enabled_,uint32_t(mode),__ATOMIC_RELEASE);
    }
    // After actual ownership. startTsc/startSteal were sampled before the
    // ticket reservation or first claim attempt; lastReleaseTsc is the
    // predecessor's release time (0 when unknown).
    void Acquired(unsigned cpu,unsigned lock,const char* caller,uint32_t line,bool contended,
                  uint64_t startTsc,uint64_t startSteal,uint64_t nowTsc,uint64_t nowSteal,
                  uint64_t lastReleaseTsc,LockStatHold* hold){
        hold->valid=false;
        if(lock>=LockStatLocks)return;
        auto* c=Row(cpu);if(!c)return;
        auto& r=c->lock[lock];
        const uint64_t wait=nowTsc>startTsc?nowTsc-startTsc:0;
        Add(r.acquisitions,1);
        if(contended){
            Add(r.contended,1);Add(r.waitCycles,wait);Max(r.maxWaitCycles,wait);Add(r.waitHist[LockStatBucket(wait)],1);
            if(nowSteal>startSteal)Add(r.waitStealNs,nowSteal-startSteal);
            if(lastReleaseTsc && nowTsc>=lastReleaseTsc){
                const uint64_t gap=nowTsc-lastReleaseTsc;
                Add(r.handoffs,1);Add(r.handoffCycles,gap);Max(r.maxHandoffCycles,gap);Add(r.handoffHist[LockStatBucket(gap)],1);
            }
        }
        const uint16_t site=FindSite(*c,caller,line,lock);
        if(site==LockStatNoSite)Add(c->siteOverflow,1);
        else {
            auto& s=c->site[site];Add(s.acquisitions,1);
            if(contended){Add(s.contended,1);Add(s.waitCycles,wait);Max(s.maxWaitCycles,wait);}
        }
        hold->acquiredTsc=nowTsc;hold->acquiredSteal=nowSteal;hold->site=site;hold->cpu=uint8_t(cpu);hold->valid=true;
    }
    // Before the releasing store. A hold that began in an older epoch is
    // dropped (its row was cleared); a hold on another CPU is impossible.
    void Released(unsigned cpu,unsigned lock,LockStatHold* hold,uint64_t nowTsc,uint64_t nowSteal){
        if(!hold->valid)return;
        hold->valid=false;
        if(lock>=LockStatLocks || cpu!=hold->cpu || cpu>=LockStatCpus)return;
        auto& c=cpu_[cpu];
        if(Load(c.epoch)!=__atomic_load_n(&epoch_,__ATOMIC_ACQUIRE))return;
        const uint64_t held=nowTsc>hold->acquiredTsc?nowTsc-hold->acquiredTsc:0;
        const uint64_t steal=nowSteal>hold->acquiredSteal?nowSteal-hold->acquiredSteal:0;
        auto& r=c.lock[lock];
        Add(r.holdCycles,held);Max(r.maxHoldCycles,held);Add(r.holdHist[LockStatBucket(held)],1);
        if(steal){Add(r.holdStealNs,steal);Add(r.stolenHolds,1);}
        if(hold->site!=LockStatNoSite){
            auto& s=c.site[hold->site];Add(s.holdCycles,held);Max(s.maxHoldCycles,held);if(steal)Add(s.holdStealNs,steal);
        }
    }

    // Reader-side aggregation into caller storage (no allocation).
    struct Merged {
        LockStatLockRow lock[LockStatLocks]{};
        LockStatSite site[LockStatSiteSlots*2]{};
        unsigned sites=0;uint64_t siteOverflow=0,mergeOverflow=0;unsigned cpusInEpoch=0;
    };
    void Read(unsigned cpus,Merged* out)const{
        *out=Merged{};
        if(cpus>LockStatCpus)cpus=LockStatCpus;
        const auto epoch=Epoch();
        for(unsigned i=0;i<cpus;++i){
            const auto& c=cpu_[i];
            if(__atomic_load_n(&c.epoch,__ATOMIC_ACQUIRE)!=epoch)continue;
            ++out->cpusInEpoch;out->siteOverflow+=Load(c.siteOverflow);
            for(unsigned l=0;l<LockStatLocks;++l){
                const auto& r=c.lock[l];auto& o=out->lock[l];
                o.acquisitions+=Load(r.acquisitions);o.contended+=Load(r.contended);
                o.waitCycles+=Load(r.waitCycles);o.holdCycles+=Load(r.holdCycles);
                if(Load(r.maxWaitCycles)>o.maxWaitCycles)o.maxWaitCycles=Load(r.maxWaitCycles);
                if(Load(r.maxHoldCycles)>o.maxHoldCycles)o.maxHoldCycles=Load(r.maxHoldCycles);
                o.waitStealNs+=Load(r.waitStealNs);o.holdStealNs+=Load(r.holdStealNs);o.stolenHolds+=Load(r.stolenHolds);
                o.handoffs+=Load(r.handoffs);o.handoffCycles+=Load(r.handoffCycles);
                if(Load(r.maxHandoffCycles)>o.maxHandoffCycles)o.maxHandoffCycles=Load(r.maxHandoffCycles);
                for(unsigned b=0;b<LockStatBuckets;++b){
                    o.waitHist[b]+=Load(r.waitHist[b]);o.holdHist[b]+=Load(r.holdHist[b]);o.handoffHist[b]+=Load(r.handoffHist[b]);
                }
            }
            for(const auto& s:c.site){
                const char* caller=__atomic_load_n(&s.caller,__ATOMIC_ACQUIRE);if(!caller)continue;
                const uint32_t line=__atomic_load_n(&s.line,__ATOMIC_RELAXED),lock=__atomic_load_n(&s.lock,__ATOMIC_RELAXED);
                LockStatSite* m=nullptr;
                for(unsigned k=0;k<out->sites;++k)
                    if(out->site[k].caller==caller && out->site[k].line==line && out->site[k].lock==lock){m=&out->site[k];break;}
                if(!m){
                    if(out->sites==LockStatSiteSlots*2){++out->mergeOverflow;continue;}
                    m=&out->site[out->sites++];m->caller=caller;m->line=line;m->lock=lock;
                }
                m->acquisitions+=Load(s.acquisitions);m->contended+=Load(s.contended);
                m->waitCycles+=Load(s.waitCycles);m->holdCycles+=Load(s.holdCycles);m->holdStealNs+=Load(s.holdStealNs);
                if(Load(s.maxWaitCycles)>m->maxWaitCycles)m->maxWaitCycles=Load(s.maxWaitCycles);
                if(Load(s.maxHoldCycles)>m->maxHoldCycles)m->maxHoldCycles=Load(s.maxHoldCycles);
            }
        }
        // Sort sites by total hold, descending (insertion sort; <= 96 entries).
        for(unsigned i=1;i<out->sites;++i){
            const LockStatSite key=out->site[i];unsigned j=i;
            while(j && out->site[j-1].holdCycles<key.holdCycles){out->site[j]=out->site[j-1];--j;}
            out->site[j]=key;
        }
    }
};

// Scheduler-side controls (scheduler.cpp). Mode arguments: -1 query, 0 off, 1 on.
void SchedulerLockStatsControl(int mode,bool reset);
bool SchedulerLockStatsEnabled();
void SchedulerLockStatsRead(SchedLockStats::Merged* output,unsigned* cpus);
bool SchedulerLockUnfairMode(int mode);   // returns the resulting mode
bool SchedulerIdleTickSkipMode(int mode); // returns the resulting mode
bool SchedulerKernelRegionLockedMode(int mode); // true: ready-lock region entry (A/B only)
}
