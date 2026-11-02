#pragma once
#include "hda_dell_stream_bdl.h"
#include "hda_dma_publish.h"

namespace brook {
// Owns one descriptor page and sixteen independent sample pages. The platform
// must return exclusively owned WB pages; allocatePage(false) transfers no
// ownership. pageAllowed must verify the writable VA, page alignment and full
// extent, and all cache aliases before clearPage is called. All
// calls are serialized by the controller owner. A fence publishes flushes;
// neither this object nor a successful flush establishes device quiescence.
// Once DMA can reference the BDL, release requires separately verified STOP,
// bus-master disable, stream reset and descriptor retirement. Quarantine is
// terminal and retains all resources until reboot.
template<class Platform> class HdaDellStreamMemory {
    Platform& p_;uint64_t physical_[17]{};void* pages_[17]{};unsigned count_=0;uint32_t line_;
    bool ready_=false,quarantined_=false,exposed_=false,retired_=false;
    void freeOwned(){
        while(count_){--count_;bool duplicate=false;for(unsigned i=0;i<count_;++i)if(physical_[i]==physical_[count_])duplicate=true;
            if(!duplicate && physical_[count_])p_.freePage(physical_[count_]);physical_[count_]=0;pages_[count_]=nullptr;}
    }
public:
    explicit HdaDellStreamMemory(Platform& p,uint32_t line):p_(p),line_(line){}
    HdaDellStreamMemory(const HdaDellStreamMemory&)=delete;
    HdaDellStreamMemory& operator=(const HdaDellStreamMemory&)=delete;
    HdaDellStreamMemory(HdaDellStreamMemory&&)=delete;
    HdaDellStreamMemory& operator=(HdaDellStreamMemory&&)=delete;
    bool allocate(bool address64){
        if(count_ || ready_ || quarantined_ || retired_ || line_<16 || line_>4096 || (line_&(line_-1)) || (4096%line_))return false;
        for(unsigned i=0;i<17;++i){
            if(!p_.allocatePage(physical_[i],pages_[i])){freeOwned();return false;}
            ++count_;if(!p_.pageAllowed(physical_[i],pages_[i])){freeOwned();return false;}
            p_.clearPage(pages_[i]);
        }
        if(!HdaDellBuildStreamBdl(static_cast<HdaToneBdlEntry*>(pages_[0]),physical_[0],pages_+1,physical_+1,address64)){
            freeOwned();return false;
        }
        ready_=true;return true;
    }
    uint64_t bdlPhysical()const{return ready_?physical_[0]:0;}
    // Pages remain owned while stopped. Recheck their mappings before any
    // descriptor or sample access in a new hardware epoch.
    bool revalidate(){
        if(!ready_ || quarantined_ || retired_ || count_!=17)return false;
        for(unsigned i=0;i<17;++i)if(!p_.pageAllowed(physical_[i],pages_[i]))return false;
        return true;
    }
    bool publishAll(){
        if(!ready_ || quarantined_)return false;
        for(void* page:pages_)if(!HdaDmaFlushRange(p_,page,4096,0,4096,line_))return false;
        return p_.fence();
    }
    // Marks the point at which BME or RUN might have been asserted, including
    // a failed command whose hardware effect cannot be determined.
    bool expose(){if(!ready_ || quarantined_)return false;exposed_=true;return true;}
    bool write(uint32_t offset,const uint8_t* data,uint32_t bytes){
        if(!ready_ || quarantined_ || offset>65536 || bytes>65536-offset)return false;
        while(bytes){const unsigned page=offset/4096+1;const uint32_t within=offset%4096;
            uint32_t n=4096-within;if(n>bytes)n=bytes;
            auto* target=static_cast<uint8_t*>(pages_[page])+within;
            for(uint32_t i=0;i<n;++i)target[i]=data?data[i]:0;
            if(data)data+=n;offset+=n;bytes-=n;
        }
        return true;
    }
    bool publish(uint32_t offset,uint32_t bytes){
        if(!ready_ || quarantined_ || offset>65536 || bytes>65536-offset)return false;
        while(bytes){const unsigned page=offset/4096+1;const uint32_t within=offset%4096;
            uint32_t n=4096-within;if(n>bytes)n=bytes;
            if(!HdaDmaFlushRange(p_,pages_[page],4096,within,n,line_))return false;
            offset+=n;bytes-=n;
        }
        return true;
    }
    bool fence(){return ready_ && !quarantined_ && p_.fence();}
    void quarantine(){quarantined_=true;}
    bool release(bool descriptorsRetired){
        if(!ready_ || quarantined_ || (exposed_ && !descriptorsRetired))return false;
        ready_=false;retired_=true;freeOwned();return true;
    }
};
}
