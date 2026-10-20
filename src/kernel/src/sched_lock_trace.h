#pragma once
#include <stdint.h>
#include "runtime_accounting.h"

namespace brook {
enum class LockTracePhase : uint64_t { None, Waiting, Acquired, Releasing, Released };
// The lock, not a Process pointer, identifies this diagnostic. Sample flags
// 0..3 indicate valid reservation/acquisition/pre-/post-release samples; bit 4
// indicates that steal was available. All timestamps are observation boundaries.
struct LockTraceRecord {
    uint64_t epoch=0,serial=0,ticket=0,phase=0,caller=0,flags=0;
    uint64_t reservedNs=0,acquiredNs=0,releaseBeforeNs=0,releaseAfterNs=0;
    uint64_t reservedSteal=0,acquiredSteal=0,releaseBeforeSteal=0,releaseAfterSteal=0;
};
struct LockTraceToken {
    uint64_t epoch=0,serial=0;
    uint32_t cpu=0,ticket=0;
    explicit operator bool()const{return epoch && serial;}
};

// Single publisher per CPU, with interrupts masked across each publication.
// Every payload access is atomic: failed sequence validation must not create a
// C++ data race. Readers make one attempt and never wait for a stopped writer.
struct LockTracePublication {
    uint64_t sequence=0;
    LockTraceRecord data{};
    bool Store(const LockTraceRecord& value) {
        const auto before=__atomic_load_n(&sequence,__ATOMIC_RELAXED);
        if((before&1) || before>=~uint64_t(0)-1)return false;
        __atomic_store_n(&sequence,before+1,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
#define LOCK_TRACE_STORE(f) __atomic_store_n(&data.f,value.f,__ATOMIC_RELAXED)
        LOCK_TRACE_STORE(epoch);LOCK_TRACE_STORE(serial);LOCK_TRACE_STORE(ticket);LOCK_TRACE_STORE(phase);
        LOCK_TRACE_STORE(caller);LOCK_TRACE_STORE(flags);
        LOCK_TRACE_STORE(reservedNs);LOCK_TRACE_STORE(acquiredNs);LOCK_TRACE_STORE(releaseBeforeNs);LOCK_TRACE_STORE(releaseAfterNs);
        LOCK_TRACE_STORE(reservedSteal);LOCK_TRACE_STORE(acquiredSteal);LOCK_TRACE_STORE(releaseBeforeSteal);LOCK_TRACE_STORE(releaseAfterSteal);
#undef LOCK_TRACE_STORE
        __atomic_store_n(&sequence,before+2,__ATOMIC_RELEASE);return true;
    }
    bool Read(LockTraceRecord* output)const {
        if(!output)return false;
        const auto before=__atomic_load_n(&sequence,__ATOMIC_ACQUIRE);
        if(before&1)return false;
        LockTraceRecord value{};
#define LOCK_TRACE_READ(f) value.f=__atomic_load_n(&data.f,__ATOMIC_RELAXED)
        LOCK_TRACE_READ(epoch);LOCK_TRACE_READ(serial);LOCK_TRACE_READ(ticket);LOCK_TRACE_READ(phase);
        LOCK_TRACE_READ(caller);LOCK_TRACE_READ(flags);
        LOCK_TRACE_READ(reservedNs);LOCK_TRACE_READ(acquiredNs);LOCK_TRACE_READ(releaseBeforeNs);LOCK_TRACE_READ(releaseAfterNs);
        LOCK_TRACE_READ(reservedSteal);LOCK_TRACE_READ(acquiredSteal);LOCK_TRACE_READ(releaseBeforeSteal);LOCK_TRACE_READ(releaseAfterSteal);
#undef LOCK_TRACE_READ
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(before!=__atomic_load_n(&sequence,__ATOMIC_RELAXED))return false;
        *output=value;return true;
    }
};
struct LockTraceCpuSnapshot {
    LockTraceRecord current{},maxWait{},maxHold{};
    uint64_t completed=0,lost=0,clockErrors=0;
    bool currentValid=false,waitValid=false,holdValid=false;
};

class SchedulerLockTrace {
    struct Cpu {
        LockTracePublication current,maxWait,maxHold;
        uint64_t completed=0,lost=0,clockErrors=0;
    } cpu_[64]{};
    // Low bit enables new reservations; upper bits identify the window.
    uint64_t state_=0,active_=0;
    static void Increment(uint64_t& counter) {
        const auto old=__atomic_load_n(&counter,__ATOMIC_RELAXED);
        if(old!=~uint64_t(0))__atomic_store_n(&counter,old+1,__ATOMIC_RELAXED);
    }
    bool Match(const LockTraceToken& token,LockTracePhase phase,LockTraceRecord* record) {
        if(!token || token.cpu>=64)return false;
        if(!cpu_[token.cpu].current.Read(record) || record->epoch!=token.epoch ||
           record->serial!=token.serial || record->ticket!=token.ticket || record->phase!=uint64_t(phase)) {
            Increment(cpu_[token.cpu].lost);return false;
        }
        return true;
    }
    static uint64_t Duration(const LockTraceRecord& r,bool hold) {
        const auto mask=hold?6u:3u;
        const auto begin=hold?r.acquiredNs:r.reservedNs;
        const auto end=hold?r.releaseBeforeNs:r.acquiredNs;
        return (r.flags&mask)==mask && end>=begin?end-begin:0;
    }
    bool Store(unsigned cpu,const LockTraceRecord& record) {
        if(cpu_[cpu].current.Store(record))return true;
        Increment(cpu_[cpu].lost);return false;
    }
public:
    uint64_t State()const{return __atomic_load_n(&state_,__ATOMIC_ACQUIRE);}
    bool Enabled()const{const auto state=State();return (state&1) && (state&~uint64_t(1))<~uint64_t(0)-3;}
    bool HasActive()const{return __atomic_load_n(&active_,__ATOMIC_ACQUIRE)!=0;}
    uint64_t ActiveMask()const{return __atomic_load_n(&active_,__ATOMIC_ACQUIRE);}
    // Off preserves the window; reset/on starts a new window without clearing
    // live publications. A failed CAS has no effect; callers may retry explicitly.
    static bool NextState(uint64_t old,bool enabled,bool reset,uint64_t* output) {
        if(!output)return false;auto next=old&~uint64_t(1);
        if(enabled || reset) {
            if(next>=~uint64_t(0)-3)return false;
            next+=2;
        }
        if(enabled)next|=1;
        *output=next;return true;
    }
    bool Control(int mode,bool reset=false) {
        if(mode < -1 || mode > 1)return false;
        auto old=State();const bool enabled=mode<0?bool(old&1):bool(mode);
        uint64_t next=0;if(!NextState(old,enabled,reset,&next))return false;
        return __atomic_compare_exchange_n(&state_,&old,next,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
    }
    LockTraceToken Reserve(unsigned cpu,uint32_t ticket,RuntimeSample sample,bool hasSteal,uint64_t caller,uint64_t expectedState=0) {
        const auto state=State();if(!(state&1) || (state&~uint64_t(1))>=~uint64_t(0)-3 || cpu>=64 || (expectedState && state!=expectedState))return {};
        LockTraceRecord old{};
        if(!cpu_[cpu].current.Read(&old) || (old.phase && old.phase!=uint64_t(LockTracePhase::Released)) || old.serial==~uint64_t(0)) {
            Increment(cpu_[cpu].lost);return {};
        }
        LockTraceRecord record{};record.epoch=state&~uint64_t(1);record.serial=old.serial+1;
        record.ticket=ticket;record.phase=uint64_t(LockTracePhase::Waiting);record.caller=caller;
        record.reservedNs=sample.wallNs;record.reservedSteal=sample.stealNs;
        record.flags=(sample.valid?1u:0u)|(hasSteal?16u:0u);
        if(!Store(cpu,record))return {};
        __atomic_fetch_or(&active_,uint64_t(1)<<cpu,__ATOMIC_RELEASE);
        return {record.epoch,record.serial,cpu,ticket};
    }
    void Acquired(const LockTraceToken& token,RuntimeSample sample) {
        LockTraceRecord record{};if(!Match(token,LockTracePhase::Waiting,&record))return;
        record.phase=uint64_t(LockTracePhase::Acquired);record.acquiredNs=sample.wallNs;
        record.acquiredSteal=sample.stealNs;if(sample.valid)record.flags|=2;
        Store(token.cpu,record);
    }
    LockTraceToken Releasing(unsigned cpu,uint32_t serving,RuntimeSample sample) {
        if(cpu>=64 || !(__atomic_load_n(&active_,__ATOMIC_ACQUIRE)&(uint64_t(1)<<cpu)))return {};
        LockTraceRecord record{};
        if(!cpu_[cpu].current.Read(&record) || record.ticket!=serving || record.phase!=uint64_t(LockTracePhase::Acquired)) {
            Increment(cpu_[cpu].lost);return {};
        }
        record.phase=uint64_t(LockTracePhase::Releasing);record.releaseBeforeNs=sample.wallNs;
        record.releaseBeforeSteal=sample.stealNs;if(sample.valid)record.flags|=4;
        if(!Store(cpu,record))return {};
        return {record.epoch,record.serial,cpu,serving};
    }
    void Released(const LockTraceToken& token,RuntimeSample sample) {
        LockTraceRecord record{};if(!Match(token,LockTracePhase::Releasing,&record))return;
        record.phase=uint64_t(LockTracePhase::Released);record.releaseAfterNs=sample.wallNs;
        record.releaseAfterSteal=sample.stealNs;if(sample.valid)record.flags|=8;
        if(Store(token.cpu,record)) {
            auto& cpu=cpu_[token.cpu];Increment(cpu.completed);
            if((record.flags&15)!=15 || record.acquiredNs<record.reservedNs ||
                record.releaseBeforeNs<record.acquiredNs || record.releaseAfterNs<record.releaseBeforeNs ||
                ((record.flags&16) && (record.acquiredSteal<record.reservedSteal ||
                    record.releaseBeforeSteal<record.acquiredSteal || record.releaseAfterSteal<record.releaseBeforeSteal)))
                Increment(cpu.clockErrors);
            for(unsigned kind=0;kind<2;++kind) {
                auto& publication=kind?cpu.maxHold:cpu.maxWait;LockTraceRecord previous{};
                if(!publication.Read(&previous) || previous.epoch!=record.epoch || Duration(record,kind)>Duration(previous,kind))
                    if(!publication.Store(record))Increment(cpu.lost);
            }
        }
        __atomic_fetch_and(&active_,~(uint64_t(1)<<token.cpu),__ATOMIC_RELEASE);
    }
    LockTraceCpuSnapshot Read(unsigned cpu)const {
        LockTraceCpuSnapshot output{};if(cpu>=64)return output;
        const auto& c=cpu_[cpu];output.currentValid=c.current.Read(&output.current);
        output.waitValid=c.maxWait.Read(&output.maxWait);output.holdValid=c.maxHold.Read(&output.maxHold);
        output.completed=__atomic_load_n(&c.completed,__ATOMIC_RELAXED);output.lost=__atomic_load_n(&c.lost,__ATOMIC_RELAXED);
        output.clockErrors=__atomic_load_n(&c.clockErrors,__ATOMIC_RELAXED);
        return output;
    }
};

struct SchedulerLockTraceSnapshot {
    uint64_t state=0,endState=0,activeMask=0;
    uint32_t nextTicket=0,servingBefore=0,servingAfter=0,cpus=0;
    LockTraceCpuSnapshot cpu[64]{};
};
bool SchedulerLockTraceControl(int mode,bool reset=false);
void SchedulerLockTraceRead(SchedulerLockTraceSnapshot* output);
}
