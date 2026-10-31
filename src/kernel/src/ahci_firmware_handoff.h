#pragma once
#include "ahci_readonly_sequence.h"
namespace brook::ahci_ro {
enum class HandoffResult : uint8_t { Ok, Invalid, CommandsActive, DeviceBusy, Interrupts, StopFailed };

// Boot-only: firmware memory remains reserved. Inspect every port before any
// controller mutation. Never drain/clear commands, replace DMA pointers, or
// restart an engine after a partial failure. Controller::port(i) returns a
// lightweight port I/O handle, and disableInterrupts() verifies GHC.IE=0.
template<class Controller>
HandoffResult QuiesceFirmware(Controller& controller,uint32_t implemented,uint32_t count,uint32_t& failurePort) {
    failurePort=0xffffffffu;
    if(!count || count>30 || (implemented>>count))return HandoffResult::Invalid;
    uint32_t initiallyStopped=0;
    for(uint32_t i=0;i<count;++i)if(implemented&(1u<<i)) {
        auto p=controller.port(i);
        const uint32_t cmd=p.read(CMD),ci=p.read(CI),sact=p.read(SACT),tfd=p.read(TFD),link=p.read(0x28);
        failurePort=i;
        if(cmd==0xffffffffu || ci==0xffffffffu || sact==0xffffffffu || tfd==0xffffffffu || link==0xffffffffu)
            return HandoffResult::Invalid;
        if(ci || sact)return HandoffResult::CommandsActive;
        // TFD may predate FIS reception on a fully stopped port. IDENTIFY is
        // still gated on a fresh readiness wait after Brook starts reception.
        if(!(cmd&(ST|FRE|CR|FR)))initiallyStopped|=1u<<i;
        else if((link&15u)==3 && (tfd&0x88u))return HandoffResult::DeviceBusy;
    }
    if(!controller.disableInterrupts())return HandoffResult::Interrupts;
    for(uint32_t i=0;i<count;++i)if(implemented&(1u<<i)) {
        auto p=controller.port(i);failurePort=i;
        p.write(IE,0);p.fence();if(p.read(IE)!=0)return HandoffResult::Interrupts;
        if(!Quiesce(p))return HandoffResult::StopFailed;
    }
    for(uint32_t i=0;i<count;++i)if(implemented&(1u<<i)) {
        auto p=controller.port(i);failurePort=i;
        const uint32_t cmd=p.read(CMD),ci=p.read(CI),sact=p.read(SACT),tfd=p.read(TFD),link=p.read(0x28);
        if(cmd==0xffffffffu || ci==0xffffffffu || sact==0xffffffffu || tfd==0xffffffffu || link==0xffffffffu ||
           (cmd&(ST|FRE|CR|FR)))return HandoffResult::Invalid;
        if(ci || sact)return HandoffResult::CommandsActive;
        if(!(initiallyStopped&(1u<<i)) && (link&15u)==3 && (tfd&0x88u))return HandoffResult::DeviceBusy;
    }
    failurePort=0xffffffffu;return HandoffResult::Ok;
}
}
