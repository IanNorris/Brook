#pragma once
#include <stdint.h>
namespace brook {
// Queue observations, not audible-drop or producer-starvation measurements.
// Gap reservations include future guard space and overlap empty-tail advances.
struct HdaDellQueueDiagnostics {
    bool enabled=false,saturated=false;
    uint64_t starts=0,stops=0,faults=0,startupGapBytes=0,laterGapEvents=0,laterGapBytes=0;
    uint64_t emptyBeforeBytes=0,emptyAfterBytes=0,publishedBytes=0,backpressure=0,publicationFailures=0;
};

// Where a terminal queue fault was raised.
enum class HdaDellFaultStage : uint8_t {None,Start,Service,Reclaim,Submit,Publish,Abort,Shutdown};
// Why. Values 1-10 are DMA position sample rejections; the rest are paths.
enum class HdaDellFaultReason : uint8_t {
    None=0,ClockUnavailable=1,PositionUnavailable=2,NotRunning=3,PositionRange=4,ClockBackwards=5,
    ServiceLate=6,PositionJump=7,DeviceError=8,ObserveRejected=9,Stalled=10,
    CursorOverflow=20,GapRejected=21,AcceptRejected=22,PublishUnsafe=23,PublishUncertain=24,PublishInvalid=25,
    PublishIo=26,ReclaimPublish=27,StartWrite=28,StartClock=29,StartRun=30,ExternalAbort=31,ShutdownStop=32};
inline const char* HdaDellFaultStageName(HdaDellFaultStage s){
    switch(s){case HdaDellFaultStage::Start:return "start";case HdaDellFaultStage::Service:return "service";
        case HdaDellFaultStage::Reclaim:return "reclaim";case HdaDellFaultStage::Submit:return "submit";
        case HdaDellFaultStage::Publish:return "publish";case HdaDellFaultStage::Abort:return "abort";
        case HdaDellFaultStage::Shutdown:return "shutdown";default:return "none";}
}
inline const char* HdaDellFaultReasonName(HdaDellFaultReason r){
    switch(r){
        case HdaDellFaultReason::ClockUnavailable:return "clock-unavailable";case HdaDellFaultReason::PositionUnavailable:return "position-unavailable";
        case HdaDellFaultReason::NotRunning:return "run-clear";case HdaDellFaultReason::PositionRange:return "lpib-out-of-range";
        case HdaDellFaultReason::ClockBackwards:return "clock-backwards";case HdaDellFaultReason::ServiceLate:return "service-late";
        case HdaDellFaultReason::PositionJump:return "position-jump";case HdaDellFaultReason::DeviceError:return "device-error";
        case HdaDellFaultReason::ObserveRejected:return "observe-rejected";case HdaDellFaultReason::Stalled:return "position-stalled";
        case HdaDellFaultReason::CursorOverflow:return "cursor-overflow";case HdaDellFaultReason::GapRejected:return "gap-rejected";
        case HdaDellFaultReason::AcceptRejected:return "accept-rejected";case HdaDellFaultReason::PublishUnsafe:return "publish-unsafe";
        case HdaDellFaultReason::PublishUncertain:return "publish-uncertain";case HdaDellFaultReason::PublishInvalid:return "publish-invalid";
        case HdaDellFaultReason::PublishIo:return "publish-io";case HdaDellFaultReason::ReclaimPublish:return "reclaim-publish";
        case HdaDellFaultReason::StartWrite:return "start-write";case HdaDellFaultReason::StartClock:return "start-clock";
        case HdaDellFaultReason::StartRun:return "start-run";case HdaDellFaultReason::ExternalAbort:return "external-abort";
        case HdaDellFaultReason::ShutdownStop:return "shutdown-stop";default:return "none";}
}
// Read-only controller/stream register snapshot. Stream fields come from the
// owned output descriptor; the rest are controller-global registers.
struct HdaDellStreamRegisters {
    bool valid=false;uint16_t command=0;
    uint32_t control=0;  // SDnCTL, 24 bits (SRST=bit 0, RUN=bit 1)
    uint8_t status=0;    // SDnSTS (BCIS=0x04, FIFOE=0x08, DESE=0x10, FIFORDY=0x20)
    uint32_t lpib=0,cbl=0;uint16_t lvi=0,fifos=0,format=0;uint32_t bdpl=0,bdpu=0;
    uint32_t intctl=0,intsts=0,wallclock=0,dplbase=0;
};
// Terminal queue fault, retained under the queue owner's lock. The sample
// fields describe the last position sample attempted (including a rejected
// one); previous* describe the last accepted sample. atNs is 0 when the
// clock itself was unavailable.
struct HdaDellQueueFault {
    bool recorded=false,muted=false,stopped=false,recoverable=false,sampledError=false,sampledRunning=false;
    HdaDellFaultStage stage=HdaDellFaultStage::None;
    HdaDellFaultReason reason=HdaDellFaultReason::None,sampleReason=HdaDellFaultReason::None;
    uint64_t ordinal=0,epoch=0,atNs=0,sampleBeforeNs=0,sampleAfterNs=0,previousSampleNs=0,lastMovementNs=0;
    uint64_t rawCompleted=0,submitted=0,completed=0,cleared=0,allowed=0;
    uint32_t position=0,previousPosition=0,delta=0;
    HdaDellStreamRegisters atFault{},afterStop{};
};
// Faults that are judgements about the software DMA clock/service timing,
// not evidence that the controller did something unexpected. Only these may
// be considered for a verified-stop recovery, and only when the at-fault
// register snapshot shows no descriptor error (DESE).
inline bool HdaDellFaultReasonRecoverable(HdaDellFaultReason r){
    switch(r){
        case HdaDellFaultReason::ServiceLate:case HdaDellFaultReason::ClockBackwards:case HdaDellFaultReason::PositionJump:
        case HdaDellFaultReason::Stalled:case HdaDellFaultReason::DeviceError:return true;
        default:return false;}
}
// Bounded fault-recovery bookkeeping (see HdaDellAudioBridge::enableFaultRecovery).
enum class HdaDellRecoveryStep : uint8_t {None,Admission,Retire,Reprepare,Queue};
struct HdaDellRecoveryStatus {
    bool enabled=false,pending=false,permanent=false;uint32_t budget=0,attempts=0,successes=0;
    HdaDellRecoveryStep lastFailure=HdaDellRecoveryStep::None;uint64_t lastRecoveryNs=0,minSpacingNs=0;
};
}
