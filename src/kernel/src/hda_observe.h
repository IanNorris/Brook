#pragma once
#include "boot_protocol/boot_protocol.h"
#include "../../drivers/intel_hda/hda_probe_session.h"
namespace brook {
struct HdaObservation {
    bool requested=false, attempted=false, mapped=false;
    const char* reason="not requested";
    uint64_t base=0;
    HdaSessionReport session{};
    bool toneRequested=false,toneAttempted=false,toneAudible=false,toneFinished=false,toneQuarantined=false;
    uint8_t toneResult=0xff;
    const char* toneReason="not requested";
    uint32_t toneAdmission=0;uint64_t tonePage=0;uint8_t tonePageStatus=0;
    uint8_t tonePrepareStage=0,toneStreamStage=0;uint32_t tonePrepareValue=0,toneStreamDetail=0;uint16_t toneCommand=0,toneFifoSize=0;
    bool toneIciFailed=false,toneFifoSampled=false;
    uint8_t tonePlayReason=0,toneProgressReason=0,toneProgressStatus=0;uint16_t toneProgressControl=0;
    uint32_t tonePlayLoops=0,tonePlayProgress=0,toneProgressFirst=0,toneProgressLast=0;uint64_t tonePlayElapsed=0;
};
// Called once on CPU 0 before AP scheduler activation or driver loading.
void HdaObserveConfigure(const BootProtocol* boot);
const HdaObservation& HdaObserveRead();
}
