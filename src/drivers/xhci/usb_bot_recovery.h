#pragma once
#include <stdint.h>
namespace brook {
enum class UsbBotReceive {Complete,Stall,Failure};
// BOT status stage only: retry the CSW receive once after clearing the halt.
// Never replay a CBW, data write, timeout, or the halted TD itself.
template<class Io> bool UsbBotReceiveCsw(Io& io) {
    auto first=io.receive();
    if(first==UsbBotReceive::Complete)return true;
    if(first!=UsbBotReceive::Stall)return false;
    if(!io.clearHalt()||!io.resetEndpoint()||!io.setDequeue())return false;
    return io.receive()==UsbBotReceive::Complete;
}
struct UsbBotDequeue {
    bool valid=false,cycle=false,skipLink=false;uint32_t index=0;uint64_t pointer=0;
};
inline UsbBotDequeue UsbBotNextDequeue(uint64_t ring,uint32_t enqueue,bool cycle,uint32_t entries) {
    UsbBotDequeue result{};
    if(entries<2||enqueue>=entries||(ring&63)||ring>~uint64_t{0}-uint64_t(entries)*16)return result;
    result.valid=true;result.skipLink=enqueue==entries-1;
    result.index=result.skipLink?0:enqueue;result.cycle=result.skipLink?!cycle:cycle;
    result.pointer=ring+uint64_t(result.index)*16+(result.cycle?1:0);return result;
}
// Diagnostic commands may send padding. Parse only CSW's relevant bytes after
// ensuring those bytes really arrived. Ordinary READ/WRITE remain exact.
inline bool UsbBotResidueValid(bool diagnostic,uint32_t requested,uint32_t transferred,uint32_t residue) {
    if(transferred>requested||residue>requested)return false;
    return diagnostic?requested-residue<=transferred:(transferred==requested&&residue==0);
}
inline bool UsbBotStatusValid(uint32_t expectedTag,uint32_t tag,uint8_t status,bool diagnostic,
                              uint32_t requested,uint32_t transferred,uint32_t residue) {
    return expectedTag==tag&&status<=1&&UsbBotResidueValid(diagnostic,requested,transferred,residue);
}

}
