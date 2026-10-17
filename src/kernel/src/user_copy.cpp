#include "user_copy.h"
#include "user_copy_fault.h"
#include "process.h"
#include "scheduler.h"
#include "memory/user_mapping.h"
#include "memory/mapping_fault.h"
#include "memory/mapping_fault_test.h"

namespace brook {
namespace {
constexpr uint64_t UserLimit=0x0000800000000000ULL;
struct Operation {
    Process* actor=ProcessCurrent();
    AddressSpaceState* space=actor?actor->addressSpace:nullptr;
    Operation() {
        if(actor)SchedulerKernelRegionEnter(actor);
        if(space)AddressSpaceRef(space);
    }
    ~Operation() {
        if(space)AddressSpaceUnref(space);
        if(actor)SchedulerKernelRegionLeave(actor);
    }
};
bool EnsurePage(Operation& operation,uint64_t page,uint32_t access) {
    auto result=UserMappingPageAccess(operation.actor,operation.space,page,access,true);
    if(result==UserPageAccess::Missing) {
        const uint64_t error=4|((access&2)?2:0);
        if(ResolveUserMappingFault(page,error)!=MappingFaultResult::Resolved)return false;
        result=UserMappingPageAccess(operation.actor,operation.space,page,access,true);
    }
    return result==UserPageAccess::Ready;
}
bool Check(Operation& operation,uint64_t address,uint64_t length,uint32_t access) {
    if(!length)return true;
    if(!operation.actor || !operation.space || operation.space->kernel ||
       address>=UserLimit || length>UserLimit-address)return false;
    const uint64_t end=address+length;
    for(uint64_t page=address&~uint64_t{4095};page<end;page+=4096)
        if(!EnsurePage(operation,page,access))return false;
    return true;
}
uint64_t Copy(void* kernel,uint64_t user,uint64_t length,bool to) {
    if(!length || !kernel)return 0;
    Operation operation;
    if(!Check(operation,user,length,to?2:1))return 0;
    if(!MappingFaultTestBarrier(operation.space,user&~uint64_t{4095},4))return 0;
    uint64_t done=0;unsigned faults=0;
    while(done<length) {
        auto* bytes=static_cast<unsigned char*>(kernel)+done;
        const uint64_t remaining=to?
            BrookCopyToUserRaw(reinterpret_cast<void*>(user+done),bytes,length-done):
            BrookCopyFromUserRaw(bytes,reinterpret_cast<void*>(user+done),length-done);
        done+=length-done-remaining;
        if(!remaining || ++faults>4)break;
        // Retry only the remaining faulting page. This runs after the assembly
        // returned normally, never from #PF or while owning a mapping/file lock.
        if(!EnsurePage(operation,(user+done)&~uint64_t{4095},to?2:1))break;
    }
    return done;
}
}
bool UserBufferReadable(uint64_t address,uint64_t length) { Operation operation;return Check(operation,address,length,1); }
bool UserBufferWritable(uint64_t address,uint64_t length) { Operation operation;return Check(operation,address,length,2); }
uint64_t CopyFromUserBytes(void* destination,uint64_t source,uint64_t length) { return Copy(destination,source,length,false); }
uint64_t CopyToUserBytes(uint64_t destination,const void* source,uint64_t length) { return Copy(const_cast<void*>(source),destination,length,true); }
bool CopyUserCString(uint64_t address,char* destination,uint32_t capacity) {
    if(!destination || !capacity)return false;
    uint32_t copied=0;
    while(copied<capacity) {
        uint64_t current;
        if(__builtin_add_overflow(address,static_cast<uint64_t>(copied),&current))return false;
        uint32_t chunk=4096-static_cast<uint32_t>(current&4095);
        if(chunk>capacity-copied)chunk=capacity-copied;
        const uint64_t got=CopyFromUserBytes(destination+copied,current,chunk);
        for(uint32_t i=0;i<got;++i)if(!destination[copied+i])return true;
        if(got!=chunk)return false;
        copied+=chunk;
    }
    destination[capacity-1]=0;return false;
}
}
