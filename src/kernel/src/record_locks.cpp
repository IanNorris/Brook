#include "record_locks.h"
#include "memory/heap.h"

namespace brook {
namespace {
constexpr uint64_t MaxOffset=0x7fffffffffffffffULL;
bool Overlap(RecordLockRange a,RecordLockRange b) { return a.begin<=b.end && b.begin<=a.end; }
bool Conflict(const RecordLockNode* node,RecordLockOwner owner,RecordLockRange range,uint16_t type) {
    return !(node->owner==owner) && Overlap(node->range,range) && (node->type==1 || type==1);
}
void Retire(RecordLockNode* node,RecordLockNode*& retired) { node->next=retired;retired=node; }
}
void RecordLockFree(RecordLockNode* nodes) {
    while(nodes) { auto* next=nodes->next;kfree(nodes);nodes=next; }
}
bool RecordLockQuery(RecordLockTable& table,RecordLockOwner owner,RecordLockRange range,
                     uint16_t type,RecordLockNode* conflict) {
    bool found=false;SpinLockAcquire(&table.lock);
    for(auto* node=table.head;node;node=node->next) {
        if(Conflict(node,owner,range,type) && (!found || node->range.begin<conflict->range.begin)) {
            *conflict=*node;conflict->next=nullptr;found=true;
        }
    }
    SpinLockRelease(&table.lock);return found;
}
void RecordLockReleaseOwner(RecordLockTable& table,RecordLockOwner owner,RecordLockNode*& retired) {
    SpinLockAcquire(&table.lock);auto** link=&table.head;
    while(auto* node=*link) {
        if(node->owner==owner) { *link=node->next;Retire(node,retired); }
        else link=&node->next;
    }
    SpinLockRelease(&table.lock);
}
RecordLockPlan::~RecordLockPlan() { RecordLockFree(spare);RecordLockFree(retired); }
bool RecordLockPlan::Prepare(unsigned count) {
    for(unsigned i=0;i<count;++i) {
        auto* node=static_cast<RecordLockNode*>(kmalloc(sizeof(RecordLockNode)));
        if(!node)return false;
        node->next=spare;spare=node;
    }
    return true;
}
int RecordLockPlan::Set(RecordLockTable& table,RecordLockOwner owner,RecordLockRange range,uint16_t type) {
    if(range.begin>range.end || range.end>MaxOffset || type>2 || !owner.pid || !owner.incarnation)return -22;
    SpinLockAcquire(&table.lock);
    unsigned needed=type==2?0:1;
    for(auto* node=table.head;node;node=node->next) {
        if(type!=2 && Conflict(node,owner,range,type)) { SpinLockRelease(&table.lock);return -11; }
        if(node->owner==owner && node->range.begin<range.begin && node->range.end>range.end)++needed;
    }
    unsigned available=0;for(auto* node=spare;node;node=node->next)++available;
    if(available<needed) { SpinLockRelease(&table.lock);return -37; }
    auto** link=&table.head;
    while(auto* node=*link) {
        if(!(node->owner==owner) || !Overlap(node->range,range)) { link=&node->next;continue; }
        if(node->range.begin<range.begin && node->range.end>range.end) {
            auto* right=spare;spare=spare->next;*right=*node;
            right->range.begin=range.end+1;right->next=node->next;
            node->range.end=range.begin-1;node->next=right;link=&right->next;
        } else if(node->range.begin<range.begin) {
            node->range.end=range.begin-1;link=&node->next;
        } else if(node->range.end>range.end) {
            node->range.begin=range.end+1;link=&node->next;
        } else { *link=node->next;Retire(node,retired); }
    }
    if(type!=2) {
        auto* inserted=spare;spare=spare->next;
        *inserted={table.head,owner,range,type};table.head=inserted;
        // Same-owner intervals never overlap. Coalesce both adjacent sides;
        // list order is immaterial (queries choose a lowest-start conflict).
        bool changed;
        do {
            changed=false;link=&inserted->next;
            while(auto* node=*link) {
                if(node->owner==owner && node->type==type &&
                   node->range.begin<=inserted->range.end+1 && inserted->range.begin<=node->range.end+1) {
                    if(node->range.begin<inserted->range.begin)inserted->range.begin=node->range.begin;
                    if(node->range.end>inserted->range.end)inserted->range.end=node->range.end;
                    *link=node->next;Retire(node,retired);changed=true;
                } else link=&node->next;
            }
        } while(changed);
    }
    SpinLockRelease(&table.lock);return 0;
}
int RecordLockNormalize(int16_t whence,int64_t start,int64_t length,
                        uint64_t position,uint64_t size,RecordLockRange* range) {
    uint64_t base=0;
    if(whence==1)base=position;else if(whence==2)base=size;else if(whence!=0)return -22;
    if(base>MaxOffset)return -75;
    int64_t first;
    if(__builtin_add_overflow(static_cast<int64_t>(base),start,&first))return -75;
    if(first<0)return -22;
    int64_t last=static_cast<int64_t>(MaxOffset);
    if(length>0) {
        if(__builtin_add_overflow(first,length-1,&last))return -75;
    } else if(length<0) {
        last=first-1;
        if(__builtin_add_overflow(first,length,&first))return -75;
        if(first<0)return -22;
    }
    *range={static_cast<uint64_t>(first),static_cast<uint64_t>(last)};return 0;
}
}
