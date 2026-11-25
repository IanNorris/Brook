#pragma once

#include "device.h"

namespace brook {
// Serialized by the MSC I/O lock, including all byte-to-block writes.
struct UsbWriteWindows {
    DeviceWriteWindow update{}, system{};
    bool heldSystem=false;

    bool CanOpenUpdate(bool systemBoot) const { return !systemBoot || heldSystem; }

    static bool Contains(const DeviceWriteWindow& window,uint64_t start,uint64_t length) {
        return window.length && length && start>=window.start && length<=window.length &&
            start-window.start<=window.length-length;
    }
    bool Allows(uint64_t start,uint64_t length) const {
        return Contains(update,start,length) || Contains(system,start,length);
    }
    int CheckUnmap(const UsbUnmapBatch& batch) const {
        if(!batch.ranges || !batch.count || batch.count>UsbUnmapMaxRanges)return -22;
        for(uint32_t i=0;i<batch.count;++i)
            if(!Contains(system,batch.ranges[i].start,batch.ranges[i].length))return -30;
        return 0;
    }
    bool CanReadDma(const UsbDmaRead& request,uint32_t block,uint32_t burst,uint64_t limit) const {
        return request.physical && request.physical<limit && request.bytes<=limit-request.physical &&
            block && request.bytes<=burst && !(request.bytes%block) && !(request.offset%block) &&
            Contains(system,request.offset,request.bytes);
    }
    bool Set(bool persistent,DeviceWriteWindow requested,uint64_t bytes,uint32_t block) {
        auto& selected=persistent?system:update;
        const auto& other=persistent?update:system;
        if(requested.length) {
            if(!block || requested.start%block || requested.length%block ||
               requested.start>bytes || requested.length>bytes-requested.start)return false;
            if(other.length && requested.start<other.start+other.length &&
               other.start<requested.start+requested.length)return false;
            if(selected.length && (selected.start!=requested.start || selected.length!=requested.length))
                return false;
        }
        selected=requested.length?requested:DeviceWriteWindow{};
        if(persistent && requested.length)heldSystem=true;
        return true;
    }
};
inline int UsbDmaTransferFailure(UsbDmaRead& request,bool quarantined) {
    request.retain=quarantined;
    return -5;
}
}
