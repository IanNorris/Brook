#include "file_page_cache.h"
#include "physical_memory.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
namespace brook {
namespace {
bool enabled=false,started=false,paused=false;
extern "C" void SchedulerSleepMs(uint32_t ms);
void ReclaimWorker(void*) {
    constexpr uint64_t Low=64*1024*1024/4096,High=128*1024*1024/4096;
    bool pressure=false;uint32_t noProgress=0;
    for(;;) {
        if(__atomic_load_n(&paused,__ATOMIC_ACQUIRE)){SchedulerSleepMs(20);continue;}
        auto free=PmmGetFreePageCount();
        if(free<Low)pressure=true;
        if(free>=High)pressure=false;
        if(!pressure || !FilePageCacheCount()){noProgress=0;SchedulerSleepMs(20);continue;}
        auto result=FilePageCacheReclaim(256);
        if(result.reclaimed){noProgress=0;SchedulerSleepMs(1);continue;}
        noProgress+=result.scanned;
        if(!result.scanned || noProgress>=65536) {
            noProgress=0;if(free>=Low)pressure=false;
            SchedulerSleepMs(20);continue;
        }
        // Owns no AS, filesystem, cache or PMM lock at a scheduling boundary.
        SchedulerYield();__asm__ volatile("sti":::"memory");
    }
}
}
void FilePageCacheEnableReclaim(){enabled=true;}
void FilePageCachePauseReclaim(bool value){__atomic_store_n(&paused,value,__ATOMIC_RELEASE);}
void FilePageCacheStartReclaimWorker() {
    if(!enabled || started)return;
    auto* process=KernelThreadCreate("file_reclaim",ReclaimWorker,nullptr,1);
    if(!process){SerialPuts("file cache: reclaim worker unavailable\n");return;}
    started=true;SchedulerAddProcess(process);
}
}
