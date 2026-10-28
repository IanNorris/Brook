#pragma once
#include <stdint.h>
namespace brook {
// Protected by the owning AS mutation mutex. Bounded representation, not a
// capacity limit: too many disjoint intervals conservatively cover the whole AS.
struct UserAccessRanges {
    struct Interval { uint64_t begin,end; };
    static constexpr unsigned Capacity=32;
    static constexpr uint64_t Limit=0x0000800000000000ULL;
    Interval intervals[Capacity];
    unsigned count;
    bool whole;
    void Reset(){count=0;whole=false;}
    bool Add(uint64_t begin,uint64_t length) {
        if(!length)return true;
        if(begin>=Limit || length>Limit-begin)return false;
        if(whole)return true;
        uint64_t end=begin+length;
        for(unsigned i=0;i<count;) {
            if(intervals[i].begin<=end && intervals[i].end>=begin) {
                if(intervals[i].begin<begin)begin=intervals[i].begin;
                if(intervals[i].end>end)end=intervals[i].end;
                intervals[i]=intervals[--count];i=0;
            } else ++i;
        }
        if(count==Capacity){whole=true;count=0;return true;}
        intervals[count++]={begin,end};return true;
    }
    bool CoversPage(uint64_t page) const {
        if(page>=Limit)return false;
        if(whole)return true;
        page&=~uint64_t{4095};
        for(unsigned i=0;i<count;++i)
            if(intervals[i].begin<page+4096 && intervals[i].end>page)return true;
        return false;
    }
};
struct UserAccessLease {
    UserAccessRanges ranges;
    UserAccessLease* next;
};
struct UserAccessRegistry {
    UserAccessLease* head=nullptr;
    void Add(UserAccessLease& lease){lease.ranges.Reset();lease.next=head;head=&lease;}
    bool Remove(UserAccessLease& lease) {
        auto** at=&head;
        while(*at && *at!=&lease)at=&(*at)->next;
        if(!*at)return false;
        *at=lease.next;lease.next=nullptr;return true;
    }
    bool CoversPage(uint64_t page) const {
        for(auto* lease=head;lease;lease=lease->next)if(lease->ranges.CoversPage(page))return true;
        return false;
    }
    uint64_t Count() const {uint64_t count=0;for(auto* at=head;at;at=at->next)++count;return count;}
};
}
