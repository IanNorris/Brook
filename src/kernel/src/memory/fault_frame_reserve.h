#pragma once
#include "address.h"
namespace brook {
// Resident metadata only; caller supplies serialization and owns one PMM
// reference per stored frame. Pop/Push transfer that reference, never duplicate.
class FaultFrameReserve {
public:
    static constexpr unsigned Capacity=8;
    bool Push(PhysicalAddress frame) {
        if(!frame || count_==Capacity)return false;
        for(unsigned i=0;i<count_;++i)if(frames_[i]==frame)return false;
        frames_[count_++]=frame;return true;
    }
    PhysicalAddress Pop() {
        if(!count_)return {};
        auto frame=frames_[--count_];frames_[count_]={};return frame;
    }
    unsigned Size() const{return count_;}
private:
    PhysicalAddress frames_[Capacity]{};
    unsigned count_=0;
};
}
