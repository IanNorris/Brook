#include "intel_bdw_buffer.h"
#include "intel_bdw_publish.h"
#include "memory/mapping_ranges.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"
#include "memory/heap.h"

namespace brook {
static constexpr uint32_t BdwGpuBufferChunkPages=256;
struct BdwGpuBuffer {
    MappingBacking* backing;
    uint64_t bytes,identity;
    uint64_t *pages,*chunks;
    uint32_t count,allocatedChunks;
    uint64_t gpuState; // 63 failure; 62 CPU transition; 61/60 handoff/consumed; 59 scanout map; low 32 GPU uses
    uint64_t handoffToken;
    bool userMappingAllowed;
};
static uint64_t g_nextBufferIdentity=1;
static uint64_t BdwNextBufferIdentity(){
    uint64_t next=__atomic_load_n(&g_nextBufferIdentity,__ATOMIC_RELAXED);
    do {if(!next || next==~uint64_t{0})return 0;}
    while(!__atomic_compare_exchange_n(&g_nextBufferIdentity,&next,next+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
    return next;
}
static void BdwGpuBufferDestroy(void* object) {
    auto* buffer=static_cast<BdwGpuBuffer*>(object);
    for(uint32_t i=0;i<buffer->allocatedChunks;++i){
        const uint32_t left=buffer->count-i*BdwGpuBufferChunkPages;
        VmmFreePages(VirtualAddress(buffer->chunks[i]),left<BdwGpuBufferChunkPages?left:BdwGpuBufferChunkPages);
    }
    kfree(buffer->chunks);kfree(buffer->pages);kfree(buffer);
}
static BdwGpuBuffer* BdwGpuBufferCreateInternal(uint64_t bytes,bool userMappingAllowed) {
    constexpr uint64_t page=4096,maxBytes=uint64_t{65536}*page,dmaLimit=uint64_t{16}<<30;
    if(!bytes || bytes>maxBytes)return nullptr;
    const uint64_t rounded=(bytes+page-1)&~(page-1);
    const uint32_t count=uint32_t(rounded/page);
    auto* buffer=static_cast<BdwGpuBuffer*>(kmalloc(sizeof(BdwGpuBuffer)));
    if(!buffer)return nullptr;
    *buffer={};buffer->count=count;buffer->bytes=rounded;buffer->identity=BdwNextBufferIdentity();buffer->userMappingAllowed=userMappingAllowed;
    if(!buffer->identity){kfree(buffer);return nullptr;}
    buffer->pages=static_cast<uint64_t*>(kmalloc(uint64_t(count)*sizeof(uint64_t)));
    if(!buffer->pages){kfree(buffer);return nullptr;}
    const uint32_t chunkCount=(count+BdwGpuBufferChunkPages-1)/BdwGpuBufferChunkPages;
    buffer->chunks=static_cast<uint64_t*>(kmalloc(uint64_t(chunkCount)*sizeof(uint64_t)));
    if(!buffer->chunks){kfree(buffer->pages);kfree(buffer);return nullptr;}
    for(uint32_t chunk=0;chunk<chunkCount;++chunk){
        const uint32_t first=chunk*BdwGpuBufferChunkPages,left=count-first;
        const uint32_t length=left<BdwGpuBufferChunkPages?left:BdwGpuBufferChunkPages;
        const auto kva=VmmAllocPages(length,VMM_WRITABLE,MemTag::Device,KernelPid);
        if(!kva.raw()){BdwGpuBufferDestroy(buffer);return nullptr;}
        buffer->chunks[buffer->allocatedChunks++]=kva.raw();
        for(uint32_t i=0;i<length;++i){
            const auto dma=VmmVirtToPhys(KernelPageTable,VirtualAddress(kva.raw()+uint64_t(i)*page)).raw();
            if(!dma || (dma&(page-1)) || dma>=dmaLimit){BdwGpuBufferDestroy(buffer);return nullptr;}
            buffer->pages[first+i]=dma;
        }
        auto* memory=reinterpret_cast<uint8_t*>(kva.raw());
        for(uint64_t i=0;i<uint64_t(length)*page;++i)memory[i]=0;
    }
    buffer->backing=MappingBackingCreate(buffer,BdwGpuBufferDestroy);
    if(!buffer->backing){BdwGpuBufferDestroy(buffer);return nullptr;}
    return buffer;
}
BdwGpuBuffer* BdwGpuBufferCreate(uint64_t bytes){return BdwGpuBufferCreateInternal(bytes,true);}
BdwGpuBuffer* BdwGpuBufferCreateHandoff(uint64_t bytes){
    auto* buffer=BdwGpuBufferCreateInternal(bytes,false);
    if(!buffer)return nullptr;
    // Publish the CPU zeroing before the buffer can enter a GPU-only handoff.
    if(!BdwGpuBufferSyncCpuCache(buffer)){BdwGpuBufferUnref(buffer);return nullptr;}
    return buffer;
}
bool BdwGpuBufferAllowsUserMapping(const BdwGpuBuffer* b){return b && b->userMappingAllowed;}
void BdwGpuBufferRef(BdwGpuBuffer* b){if(b)MappingBackingRef(b->backing);}
void BdwGpuBufferUnref(BdwGpuBuffer* b){if(b)MappingBackingUnref(b->backing);}
uint64_t BdwGpuBufferBytes(const BdwGpuBuffer* b){return b?b->bytes:0;}
uint64_t BdwGpuBufferIdentity(const BdwGpuBuffer* b){return b?b->identity:0;}
uint64_t BdwGpuBufferPageKernelAddress(const BdwGpuBuffer* b,uint32_t page){
    return b && page<b->count ? b->chunks[page/BdwGpuBufferChunkPages]+uint64_t(page%BdwGpuBufferChunkPages)*4096 : 0;
}
const uint64_t* BdwGpuBufferPages(const BdwGpuBuffer* b,uint32_t& count){count=b?b->count:0;return b?b->pages:nullptr;}
MappingBacking* BdwGpuBufferBacking(BdwGpuBuffer* b){return b?b->backing:nullptr;}
bool BdwGpuBufferBeginGpuUse(BdwGpuBuffer* b){
    if(!b)return false;
    uint64_t current=__atomic_load_n(&b->gpuState,__ATOMIC_ACQUIRE);
    do {if((current>>60) || uint32_t(current)==~uint32_t{0})return false;}
    while(!__atomic_compare_exchange_n(&b->gpuState,&current,current+1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
    return true;
}
bool BdwGpuBufferReserveHandoff(BdwGpuBuffer* b,uint64_t token){
    if(!b || !token)return false;
    uint64_t empty=0;
    if(!__atomic_compare_exchange_n(&b->handoffToken,&empty,token,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    uint64_t idle=0;
    if(__atomic_compare_exchange_n(&b->gpuState,&idle,uint64_t{1}<<61,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return true;
    __atomic_store_n(&b->handoffToken,0,__ATOMIC_RELEASE);return false;
}
bool BdwGpuBufferBeginHandoffGpuUse(BdwGpuBuffer* b,uint64_t token){
    if(!b || !token || __atomic_load_n(&b->handoffToken,__ATOMIC_ACQUIRE)!=token)return false;
    uint64_t reserved=uint64_t{1}<<61;
    return __atomic_compare_exchange_n(&b->gpuState,&reserved,reserved+(uint64_t{1}<<60)+1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
bool BdwGpuBufferHandoffMatches(const BdwGpuBuffer* b,uint64_t token){
    return b && token && __atomic_load_n(&b->handoffToken,__ATOMIC_ACQUIRE)==token &&
           __atomic_load_n(&b->gpuState,__ATOMIC_ACQUIRE)==(uint64_t{1}<<61);
}
bool BdwGpuBufferHandoffCompleted(const BdwGpuBuffer* b,uint64_t token){
    return b && token && __atomic_load_n(&b->handoffToken,__ATOMIC_ACQUIRE)==token &&
           __atomic_load_n(&b->gpuState,__ATOMIC_ACQUIRE)==((uint64_t{1}<<61)|(uint64_t{1}<<60));
}
bool BdwGpuBufferBeginScanoutMapping(BdwGpuBuffer* b,uint64_t token){
    if(!b || !token || __atomic_load_n(&b->handoffToken,__ATOMIC_ACQUIRE)!=token)return false;
    uint64_t complete=(uint64_t{1}<<61)|(uint64_t{1}<<60);
    return __atomic_compare_exchange_n(&b->gpuState,&complete,complete|(uint64_t{1}<<59),false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
bool BdwGpuBufferEndScanoutMapping(BdwGpuBuffer* b,uint64_t token){
    if(!b || !token || __atomic_load_n(&b->handoffToken,__ATOMIC_ACQUIRE)!=token)return false;
    uint64_t mapped=(uint64_t{1}<<61)|(uint64_t{1}<<60)|(uint64_t{1}<<59);
    return __atomic_compare_exchange_n(&b->gpuState,&mapped,mapped&~(uint64_t{1}<<59),false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
bool BdwGpuBufferScanoutMappingMatches(const BdwGpuBuffer* b,uint64_t token){
    return b && token && __atomic_load_n(&b->handoffToken,__ATOMIC_ACQUIRE)==token &&
        __atomic_load_n(&b->gpuState,__ATOMIC_ACQUIRE)==((uint64_t{1}<<61)|(uint64_t{1}<<60)|(uint64_t{1}<<59));
}
bool BdwGpuBufferReleaseHandoff(BdwGpuBuffer* b,uint64_t token){
    if(!b || !token || __atomic_load_n(&b->handoffToken,__ATOMIC_ACQUIRE)!=token)return false;
    uint64_t reserved=__atomic_load_n(&b->gpuState,__ATOMIC_ACQUIRE);
    if(reserved!=(uint64_t{1}<<61) && reserved!=((uint64_t{1}<<61)|(uint64_t{1}<<60)))return false;
    // A transient CPU-transition gate prevents a new owner from overlapping
    // token retirement. Sticky failure and active GPU use both prevent release.
    if(!__atomic_compare_exchange_n(&b->gpuState,&reserved,uint64_t{1}<<62,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    __atomic_store_n(&b->handoffToken,0,__ATOMIC_RELEASE);
    const uint64_t prior=__atomic_fetch_and(&b->gpuState,~(uint64_t{1}<<62),__ATOMIC_ACQ_REL);
    return !(prior>>63);
}
bool BdwGpuBufferBeginCpuTransition(BdwGpuBuffer* b){
    if(!b)return false;uint64_t expected=0;
    return __atomic_compare_exchange_n(&b->gpuState,&expected,uint64_t{1}<<62,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
bool BdwGpuBufferEndCpuTransition(BdwGpuBuffer* b){
    if(!b)return false;const uint64_t prior=__atomic_fetch_and(&b->gpuState,~(uint64_t{1}<<62),__ATOMIC_ACQ_REL);
    return (prior&(uint64_t{1}<<62))!=0 && !(prior>>63);
}
bool BdwGpuBufferEndGpuUse(BdwGpuBuffer* b){
    if(!b)return false;
    uint64_t current=__atomic_load_n(&b->gpuState,__ATOMIC_ACQUIRE);
    do {if(!uint32_t(current))return false;}
    while(!__atomic_compare_exchange_n(&b->gpuState,&current,current-1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
    return true;
}
uint32_t BdwGpuBufferGpuUses(const BdwGpuBuffer* b){return b?uint32_t(__atomic_load_n(&b->gpuState,__ATOMIC_ACQUIRE)):0;}
void BdwGpuBufferMarkGpuFailed(BdwGpuBuffer* b){if(b)__atomic_fetch_or(&b->gpuState,uint64_t{1}<<63,__ATOMIC_ACQ_REL);}
bool BdwGpuBufferGpuFailed(const BdwGpuBuffer* b){return b && (__atomic_load_n(&b->gpuState,__ATOMIC_ACQUIRE)>>63);}
void BdwGpuBufferGpuStatus(const BdwGpuBuffer* b,uint32_t& uses,bool& failed){
    const uint64_t state=b?__atomic_load_n(&b->gpuState,__ATOMIC_ACQUIRE):0;
    uses=uint32_t(state);failed=(state>>63)!=0;
}
bool BdwGpuBufferSyncCpuCache(BdwGpuBuffer* b){
    if(!b)return false;
    constexpr uint32_t group=16;
    for(uint32_t first=0;first<b->count;first+=group){
        const uint32_t count=b->count-first<group?b->count-first:group;
        BdwOwnedPage pages[group]{};
        for(uint32_t i=0;i<count;++i)pages[i]={b->pages[first+i],reinterpret_cast<uint8_t*>(BdwGpuBufferPageKernelAddress(b,first+i))};
#ifndef BROOK_HOST_TEST
        uint64_t flags;__asm__ volatile("pushfq; popq %0; cli":"=r"(flags)::"memory");
#endif
        uint32_t a,ebx,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(ebx),"=c"(c),"=d"(d):"a"(1),"c"(0));
        struct Flush {void fence(){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
            void line(void* p){__asm__ volatile("clflush (%0)"::"r"(p):"memory");}} flush;
        const bool ok=(d&(1u<<19)) && BdwPublishCpuPages(flush,pages,count,((ebx>>8)&255)*8);
#ifndef BROOK_HOST_TEST
        if(flags&(1ull<<9))__asm__ volatile("sti":::"memory");
#endif
        if(!ok)return false;
    }
    return true;
}
bool BdwGpuBufferPrepareHandoffRead(BdwGpuBuffer* b,uint64_t token,uint64_t offset,uint64_t bytes) {
    if(!b || b->userMappingAllowed || !bytes || offset>b->bytes || bytes>b->bytes-offset ||
       !BdwGpuBufferHandoffCompleted(b,token))return false;
    uint32_t a,ebx,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(ebx),"=c"(c),"=d"(d):"a"(1),"c"(0));
    const uint32_t line=((ebx>>8)&255)*8;
    if(!(d&(1u<<19)) || line<16 || line>4096 || (line&(line-1)) || (4096%line))return false;
    const uint64_t end=(offset+bytes+line-1)&~uint64_t(line-1);
    offset&=~uint64_t(line-1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while(offset<end) {
        const uint64_t address=BdwGpuBufferPageKernelAddress(b,uint32_t(offset/4096));
        const uint32_t start=uint32_t(offset%4096);
        const uint32_t stop=end-offset<4096-start?start+uint32_t(end-offset):4096;
        for(uint32_t i=start;i<stop;i+=line)
            __asm__ volatile("clflush (%0)"::"r"(address+i):"memory");
        offset+=stop-start;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return BdwGpuBufferHandoffCompleted(b,token);
}
BdwCpuTransitionResult BdwGpuBufferTryCpuTransition(BdwGpuBuffer* b){
    if(!b)return BdwCpuTransitionResult::Failed;
    if(!BdwGpuBufferBeginCpuTransition(b))
        return BdwGpuBufferGpuFailed(b)?BdwCpuTransitionResult::Failed:BdwCpuTransitionResult::Busy;
    const bool flushed=BdwGpuBufferSyncCpuCache(b);
    const bool released=BdwGpuBufferEndCpuTransition(b);
    return flushed && released?BdwCpuTransitionResult::Ready:BdwCpuTransitionResult::Failed;
}
bool BdwGpuBufferPin::Acquire(BdwGpuBuffer* b){if(buffer_ || !b)return false;BdwGpuBufferRef(b);buffer_=b;return true;}
bool BdwGpuBufferPin::Retire(bool safelyStopped){if(!buffer_ || !safelyStopped)return false;
    auto* b=buffer_;buffer_=nullptr;BdwGpuBufferUnref(b);return true;}
}
