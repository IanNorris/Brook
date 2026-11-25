#pragma once

#include "device.h"
#include "ssd_trim_plan.h"

namespace brook {
struct UsbTrimBatchPlan {
    DeviceWriteWindow ranges[UsbUnmapMaxRanges]{};
    uint64_t total=0,cap;
    uint32_t count=0,sectors=0,maxSectors,maxRanges;

    UsbTrimBatchPlan(UsbUnmapLimits limits,uint64_t requested)
        :cap(requested),maxSectors((limits.maxSectors<UsbUnmapMaxSectors
             ?limits.maxSectors:UsbUnmapMaxSectors)&~7u),
         maxRanges(limits.maxRanges<UsbUnmapMaxRanges?limits.maxRanges:UsbUnmapMaxRanges) {}

    template<class Submit> bool Drain(Submit submit) {
        if(count && !submit(ranges,count))return false;
        count=0;
        sectors=0;
        return true;
    }
    template<class Submit> bool Add(TrimRange range,Submit submit) {
        if(!maxSectors || !maxRanges || (range.sectors&7u))return false;
        if(cap && range.sectors>cap-total)range.sectors=cap-total;
        while(range.sectors) {
            if((count==maxRanges || sectors==maxSectors) && !Drain(submit))return false;
            const uint32_t room=maxSectors-sectors;
            const uint32_t take=range.sectors<room?uint32_t(range.sectors):room;
            ranges[count++]={range.lba*512,uint64_t(take)*512};
            sectors+=take;
            total+=take;
            range.lba+=take;
            range.sectors-=take;
        }
        return true;
    }
};
}
