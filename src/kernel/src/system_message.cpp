#include "system_message.h"
#include "spinlock.h"
namespace brook {
namespace {
constexpr unsigned Capacity=16;
BrookSystemMessage messages[Capacity]{};
uint64_t sequence=0;
IrqSpinLock lock;
bool terminated(const char* data,unsigned size) {
    for(unsigned i=0;i<size;++i)if(!data[i])return true;
    return false;
}
}
bool SystemMessagePublish(const BrookSystemMessage& event) {
    if(event.version!=BROOK_SYSTEM_MESSAGE_VERSION || event.size!=sizeof(event) ||
       event.code<BROOK_MESSAGE_APP_CLOSED_MEMORY || event.code>BROOK_MESSAGE_TERMINATION_REQUESTED ||
       !terminated(event.app_name,sizeof(event.app_name)) || !terminated(event.executable,sizeof(event.executable)))return false;
    const auto flags=IrqSpinLockAcquire(&lock);
    if(sequence==~uint64_t{0}){IrqSpinLockRelease(&lock,flags);return false;}
    const uint64_t next=sequence+1;
    messages[(next-1)%Capacity]=event;
    messages[(next-1)%Capacity].sequence=next;
    sequence=next;
    IrqSpinLockRelease(&lock,flags);return true;
}
bool SystemMessageReadAfter(uint64_t after,uint32_t uid,BrookSystemMessage* event) {
    if(!event)return false;
    *event={};
    const auto flags=IrqSpinLockAcquire(&lock);
    const uint64_t first=sequence>=Capacity?sequence-Capacity+1:1;
    uint64_t next=after<first?first:after==~uint64_t{0}?0:after+1;
    bool found=false;
    while(next && next<=sequence) {
        const auto& candidate=messages[(next-1)%Capacity];
        if(uid==0 || candidate.victim_uid==uid || candidate.victim_uid==~uint32_t{0}) {
            *event=candidate;found=true;break;
        }
        if(next==~uint64_t{0})break;
        ++next;
    }
    IrqSpinLockRelease(&lock,flags);return found;
}
uint64_t SystemMessageLastSequence() {
    const auto flags=IrqSpinLockAcquire(&lock);const auto value=sequence;
    IrqSpinLockRelease(&lock,flags);return value;
}
}
