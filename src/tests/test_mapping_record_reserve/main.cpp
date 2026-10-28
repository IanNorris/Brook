#include <cassert>
#include <cstdlib>
#include <set>
#include "memory/mapping_transaction.h"
#include "process.h"
#include "panic.h"
using namespace brook;
static int failAfter=-1,depth=0;
static unsigned attempts=0;
static std::set<void*> heap;
extern "C" void KernelPanic(const char*,...){std::abort();}
namespace brook {
extern "C" void* kmalloc(uint64_t size) {
    ++attempts;if(!failAfter)return nullptr;if(failAfter>0)--failAfter;
    void* p=std::malloc(size);if(p)heap.insert(p);return p;
}
extern "C" void kfree(void* p){if(p){assert(heap.erase(p)==1);std::free(p);}}
void SchedulerKernelRegionEnter(Process*){++depth;}
void SchedulerKernelRegionLeave(Process*){assert(depth>0);--depth;}
void AddressSpaceRef(AddressSpaceState* p){++p->references;}
void AddressSpaceUnref(AddressSpaceState* p){assert(p->references>1);--p->references;}
void MappingBackingRef(MappingBacking* p){assert(!p);}
void MappingBackingUnref(MappingBacking* p){assert(!p);}
void PmmUnrefPage(PhysicalAddress){assert(false);}
PhysicalAddress VmmLeafPhysical(uint64_t raw){return PhysicalAddress(raw&0x000ffffffffff000ULL);}
void TlbShootdownFull(uint64_t,uint64_t){assert(false);}
void MappingRangePlan::Finish(){}
MappingRangePlan::~MappingRangePlan(){}
void FileMappingGateSet::Finish(){}
void FileMappingGateSet::Unlock(){}
FileMappingGateSet::~FileMappingGateSet(){}
}
int main() {
    Process actor{};AddressSpaceState space{};space.references=1;
    failAfter=0;
    {MappingTransaction op(&actor,&space);assert(op.Prepare(1,1,1));assert(!op.PreparedPage(0));}
    assert(!attempts && heap.empty() && !depth && space.references==1);
    for(unsigned which=0;which<3;++which) {
        MappingTransaction op(&actor,&space);
        assert(!op.Prepare(which==0?2:0,which==1?2:0,which==2?2:0));
    }
    assert(heap.empty() && !depth && space.references==1);
    failAfter=1;
    {MappingTransaction op(&actor,&space);assert(!op.Prepare(2,2,2));assert(heap.size()==1);}
    assert(heap.empty() && !depth && space.references==1);
    failAfter=-1;
    {MappingTransaction op(&actor,&space);assert(op.Prepare(2,2,2));assert(heap.size()==3);assert(!op.PreparedPage(0));}
    assert(heap.empty() && !depth && space.references==1);
}
