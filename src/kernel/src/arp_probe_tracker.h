#pragma once
#include <stdint.h>

namespace brook {
// Caller serializes all operations and supplies monotonic millisecond ticks.
// Interface IDs must identify a stable interface/configuration lifetime. The
// tracker owns no interface, packet, waiter or ARP-cache reference; callers
// pin interface lifetime around transmit and revalidate completion tickets.
template<unsigned Capacity> class ArpProbeTracker {
    static_assert(Capacity>0,"ARP probe tracker must have at least one entry");
public:
    enum class Status : uint8_t { Pending, Failed, Full, Invalid };
    struct Ticket { unsigned slot=Capacity; uint64_t generation=0, interfaceId=0; uint32_t ip=0; };
    struct Result { Status status; int error; };
private:
    enum class State : uint8_t { Free, Waiting, Submitting, Failed };
    struct Entry {
        State state=State::Free;
        uint64_t generation=0, interfaceId=0, deadline=0, next=0, failedUntil=0;
        uint32_t ip=0; unsigned attempts=0; int error=0;
    } entries_[Capacity]{};
    uint64_t generation_=0;
    unsigned cursor_=0;
    static uint64_t Add(uint64_t a,uint64_t b) { return a>~uint64_t(0)-b ? ~uint64_t(0) : a+b; }
    static void Fail(Entry& e,int error,uint64_t now) {
        e.state=State::Failed;e.error=error;e.failedUntil=Add(now,500);
    }
    static void Expire(Entry& e,uint64_t now) {
        if(e.state!=State::Free && e.state!=State::Failed && now>=e.deadline) Fail(e,-110,now);
        if(e.state==State::Failed && now>=e.failedUntil) e.state=State::Free;
    }
public:
    Result Request(uint64_t interfaceId,uint32_t ip,uint64_t now,uint64_t timeout) {
        if(!interfaceId || !ip || !timeout)return {Status::Invalid,-22};
        for(auto& e:entries_) {
            Expire(e,now);
            if(e.state!=State::Free && e.interfaceId==interfaceId && e.ip==ip)
                return {e.state==State::Failed ? Status::Failed : Status::Pending,e.error};
        }
        for(auto& e:entries_) if(e.state==State::Free) {
            if(generation_==~uint64_t(0))return {Status::Full,-75};
            e={};e.state=State::Waiting;e.generation=++generation_;e.interfaceId=interfaceId;
            e.ip=ip;e.deadline=Add(now,timeout);e.next=now;
            return {Status::Pending,0};
        }
        return {Status::Full,-11};
    }
    bool Next(uint64_t now,Ticket* ticket) {
        if(!ticket)return false;
        for(unsigned n=0;n<Capacity;++n) {
            const unsigned i=(cursor_+n)%Capacity;auto& e=entries_[i];Expire(e,now);
            if(e.state!=State::Waiting || now<e.next)continue;
            if(e.attempts>=3){Fail(e,-110,now);continue;}
            e.state=State::Submitting;*ticket={i,e.generation,e.interfaceId,e.ip};
            cursor_=(i+1)%Capacity;return true;
        }
        return false;
    }
    bool Complete(const Ticket& ticket,int result,uint64_t now) {
        if(ticket.slot>=Capacity)return false;
        auto& e=entries_[ticket.slot];
        if(e.state!=State::Submitting || e.generation!=ticket.generation ||
           e.interfaceId!=ticket.interfaceId || e.ip!=ticket.ip)return false;
        if(now>=e.deadline){Fail(e,-110,now);return true;}
        if(result==0){++e.attempts;e.next=Add(now,500);e.state=State::Waiting;}
        else if(result==-11){e.next=Add(now,10);e.state=State::Waiting;}
        else Fail(e,result<0?result:-5,now);
        return true;
    }
    // Call only after a usable cache entry has actually been retained.
    void Resolved(uint64_t interfaceId,uint32_t ip) {
        for(auto& e:entries_) if(e.state!=State::Free && e.interfaceId==interfaceId && e.ip==ip)
            e.state=State::Free;
    }
};
}
