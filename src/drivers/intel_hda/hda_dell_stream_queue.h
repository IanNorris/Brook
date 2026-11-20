#pragma once
#include "hda_dell_stream_publish.h"
#include "hda_dell_queue_diagnostics.h"
#include "../../kernel/src/audio_dma_clock.h"

namespace brook {
// Injected streaming core. The owner
// serializes all calls and owns the controller and all ring pages. Io methods:
// nowNs(), position(raw,error,running), write(offset,src,n), publish(offset,n),
// fence(), start(), mute(), stop(), quarantine(). write(nullptr) writes silence.
// Before start(), the caller has prepared and published an immutable BDL and
// configured the validated 44.1kHz route with the speaker muted. The physical
// guard and a service deadline must be established before hardware use.
// mute() is bounded best-effort; stop() verifies RUN-clear and disables DMA.
// A missed service deadline can be detected but cannot undo stale audio which
// hardware already fetched; callers must not claim a hard real-time guarantee.
struct HdaDellReceipt {uint64_t epoch=0,begin=0;uint32_t accepted=0;int error=0;};
// Optional read-only register snapshot for fault records (owner-provided).
template<class T,class=void> struct HdaDellHasFaultRegisters {static constexpr bool value=false;};
template<class T> struct HdaDellHasFaultRegisters<T,decltype(void(static_cast<T*>(nullptr)->faultRegisters(*static_cast<HdaDellStreamRegisters*>(nullptr))))> {
    static constexpr bool value=true;};
template<class Io> class HdaDellStreamQueue {
    static constexpr uint32_t Ring=AudioDmaClock::RingBytes;
    Io& io_;AudioDmaClock clock_{};uint64_t cleared_=0,lastMovementNs_=0,clockUncertaintyNs_;uint32_t line_,guard_,maxBurst_;
    bool initialized_=false,failed_=false,closed_=false;
    bool hasPublished_=false;HdaDellQueueDiagnostics diagnostics_{};
    // Fault evidence. why_/ioFailed_ are reset at each top-level operation.
    HdaDellFaultReason why_=HdaDellFaultReason::None;bool ioFailed_=false,allowRecovery_=false,recoverable_=false;
    uint64_t sampleBefore_=0,sampleAfter_=0,sampleAllowed_=0,faultCount_=0;uint32_t samplePosition_=0,sampleDelta_=0;
    bool sampleError_=false,sampleRunning_=false;HdaDellQueueFault first_{},last_{};
    bool reject(HdaDellFaultReason reason){why_=reason;return false;}
    void resetEvidence(){why_=HdaDellFaultReason::None;ioFailed_=false;}
    void capture(HdaDellStreamRegisters& r){
        if constexpr(HdaDellHasFaultRegisters<Io>::value)io_.faultRegisters(r);else r=HdaDellStreamRegisters{};}
    HdaDellFaultReason publishReason(HdaStreamPublishResult r)const{
        if(ioFailed_)return HdaDellFaultReason::PublishIo;
        return r==HdaStreamPublishResult::Unsafe?HdaDellFaultReason::PublishUnsafe:
            r==HdaStreamPublishResult::Invalid?HdaDellFaultReason::PublishInvalid:HdaDellFaultReason::PublishUncertain;}
    // A recovery candidate must be a software timing judgement, with mute and
    // RUN-clear/BME-off verified, no descriptor error latched, and register
    // evidence captured. Anything else stays terminal.
    static bool recoverableFault(const HdaDellQueueFault& f,bool ioFailed){
        if(!f.muted || !f.stopped || ioFailed || !f.atFault.valid || !f.afterStop.valid ||
           (f.atFault.status&0x10) || (f.afterStop.status&0x10) || (f.afterStop.control&3) || f.afterStop.command!=2)return false;
        switch(f.stage){
            case HdaDellFaultStage::Service:return HdaDellFaultReasonRecoverable(f.reason);
            case HdaDellFaultStage::Publish:case HdaDellFaultStage::Reclaim:
                return (f.reason==HdaDellFaultReason::PublishUncertain || f.reason==HdaDellFaultReason::PublishUnsafe) &&
                    (f.sampleReason==HdaDellFaultReason::None || HdaDellFaultReasonRecoverable(f.sampleReason));
            default:return false;}
    }
    void add(uint64_t& value,uint64_t amount=1){
        if(amount>~uint64_t(0)-value){value=~uint64_t(0);diagnostics_.saturated=true;}
        else value+=amount;
    }
    struct Publisher {
        HdaDellStreamQueue& q;
        bool sample(uint64_t& c){return q.sample(c);}
        bool write(uint32_t o,const uint8_t* p,uint32_t n){if(q.io_.write(o,p,n))return true;q.ioFailed_=true;return false;}
        bool publish(uint32_t o,uint32_t n){if(q.io_.publish(o,n))return true;q.ioFailed_=true;return false;}
        bool fence(){if(q.io_.fence())return true;q.ioFailed_=true;return false;}
    };
    // Records the first terminal fault of this epoch: register evidence before
    // and after the bounded mute/stop. Quarantine follows unless the fault is
    // an admitted recovery candidate (see recoverableFault).
    bool fail(HdaDellFaultStage stage,HdaDellFaultReason reason){
        if(failed_)return false;
        HdaDellQueueFault f{};f.recorded=true;f.ordinal=__atomic_add_fetch(&faultCount_,uint64_t(1),__ATOMIC_RELAXED);f.stage=stage;f.reason=reason;f.sampleReason=why_;
        f.epoch=clock_.epoch;f.sampleBeforeNs=sampleBefore_;f.sampleAfterNs=sampleAfter_;f.position=samplePosition_;
        f.delta=sampleDelta_;f.allowed=sampleAllowed_;f.sampledError=sampleError_;f.sampledRunning=sampleRunning_;
        f.previousSampleNs=clock_.SampleBeforeNs();f.previousPosition=clock_.Position();f.lastMovementNs=lastMovementNs_;
        f.rawCompleted=clock_.RawCompleted();f.submitted=clock_.submitted;f.completed=clock_.completed;f.cleared=cleared_;
        capture(f.atFault);f.atNs=io_.nowNs();
        if(diagnostics_.enabled)add(diagnostics_.faults);failed_=true;clock_.Fault();
        f.muted=io_.mute();f.stopped=io_.stop();capture(f.afterStop);
        f.recoverable=allowRecovery_ && recoverableFault(f,ioFailed_);
        recoverable_=f.recoverable;if(!recoverable_)io_.quarantine();
        last_=f;if(!first_.recorded)first_=f;
        return false;
    }
    bool sample(uint64_t& raw){
        sampleBefore_=sampleAfter_=sampleAllowed_=0;samplePosition_=sampleDelta_=0;sampleError_=sampleRunning_=false;
        const uint64_t before=io_.nowNs();uint32_t position=0;bool error=false,running=false;sampleBefore_=before;
        if(!before)return reject(HdaDellFaultReason::ClockUnavailable);
        if(!io_.position(position,error,running))return reject(HdaDellFaultReason::PositionUnavailable);
        samplePosition_=position;sampleError_=error;sampleRunning_=running;
        if(!running)return reject(HdaDellFaultReason::NotRunning);
        const uint64_t after=io_.nowNs();sampleAfter_=after;
        const uint64_t previous=clock_.RawCompleted();
        if(position>Ring)return reject(HdaDellFaultReason::PositionRange);
        if(after<clock_.SampleBeforeNs())return reject(after?HdaDellFaultReason::ClockBackwards:HdaDellFaultReason::ClockUnavailable);
        const uint64_t elapsed=after-clock_.SampleBeforeNs();
        if(clockUncertaintyNs_>=AudioDmaClock::MaxSampleNs ||
           elapsed>=AudioDmaClock::MaxSampleNs-clockUncertaintyNs_)return reject(HdaDellFaultReason::ServiceLate);
        const uint32_t normalized=position==Ring?0:position;
        const uint32_t delta=(normalized+Ring-clock_.Position())%Ring;
        const uint64_t allowed=(elapsed+clockUncertaintyNs_)*AudioDmaClock::BytesPerSecond/1000000000ULL+maxBurst_;
        sampleDelta_=delta;sampleAllowed_=allowed;
        if(delta>allowed)return reject(HdaDellFaultReason::PositionJump); // impossible jump or unobserved lap
        const uint64_t previousBefore=clock_.SampleBeforeNs();
        if(!clock_.Observe(normalized,before,after,error))
            return reject(error?HdaDellFaultReason::DeviceError:before<previousBefore?HdaDellFaultReason::ClockBackwards:
                after-previousBefore>=AudioDmaClock::MaxSampleNs?HdaDellFaultReason::ServiceLate:HdaDellFaultReason::ObserveRejected);
        raw=clock_.RawCompleted();
        if(diagnostics_.enabled && raw>previous && raw>clock_.submitted){
            const uint64_t floor=previous>clock_.submitted?previous:clock_.submitted;
            add(hasPublished_?diagnostics_.emptyAfterBytes:diagnostics_.emptyBeforeBytes,raw-floor);
        }
        if(raw!=previous)lastMovementNs_=after;
        else if(after<lastMovementNs_ || after-lastMovementNs_>=150000000ULL)return reject(HdaDellFaultReason::Stalled);
        return true;
    }
    bool reclaim(uint64_t cursor){
        if(cursor<guard_)return true;
        const uint64_t end=(cursor-guard_)&~uint64_t(line_-1);
        if(end<=cleared_)return true;
        if(end>~uint64_t(0)-Ring || cleared_>~uint64_t(0)-Ring)return fail(HdaDellFaultStage::Reclaim,HdaDellFaultReason::CursorOverflow);
        uint64_t first=cleared_+Ring,last=end+Ring;
        if(first<clock_.submitted)first=clock_.submitted;
        if(first<last){
            const uint64_t count=last-first;
            if(count>Ring || (first&3) || (count&3))return fail(HdaDellFaultStage::Reclaim,HdaDellFaultReason::ReclaimPublish);
            Publisher p{*this};
            const auto r=HdaStreamPublish(p,0,first,nullptr,uint32_t(count),Ring,line_,guard_);
            if(r!=HdaStreamPublishResult::Published)return fail(HdaDellFaultStage::Reclaim,publishReason(r));
        }
        cleared_=end;return true;
    }
public:
    HdaDellStreamQueue(Io& io,uint32_t line,uint32_t guard,uint32_t maxBurst=1024,uint64_t clockUncertaintyNs=0,bool diagnostics=false):io_(io),clockUncertaintyNs_(clockUncertaintyNs),line_(line),guard_(guard),maxBurst_(maxBurst){diagnostics_.enabled=diagnostics;}
    HdaDellStreamQueue(const HdaDellStreamQueue&)=delete;
    HdaDellStreamQueue& operator=(const HdaDellStreamQueue&)=delete;
    HdaDellStreamQueue(HdaDellStreamQueue&&)=delete;
    HdaDellStreamQueue& operator=(HdaDellStreamQueue&&)=delete;
    bool start(){
        if(initialized_ || failed_ || closed_ || line_<16 || line_>4096 || (line_&(line_-1)) ||
           (4096%line_) || !maxBurst_ || maxBurst_>guard_ || guard_-maxBurst_<line_ || guard_>=Ring/4)return false;
        // Initial zeroes must be visible to DMA before RUN; no partial
        // initialization can register or expose an active driver.
        resetEvidence();
        if(!io_.write(0,nullptr,Ring) || !io_.publish(0,Ring) || !io_.fence())return fail(HdaDellFaultStage::Start,HdaDellFaultReason::StartWrite);
        const uint64_t now=io_.nowNs();if(!clock_.Start(0,now))return fail(HdaDellFaultStage::Start,HdaDellFaultReason::StartClock);lastMovementNs_=now;
        if(!io_.start())return fail(HdaDellFaultStage::Start,HdaDellFaultReason::StartRun);
        initialized_=true;if(diagnostics_.enabled)add(diagnostics_.starts);return true;
    }
    bool service(){
        if(!initialized_ || failed_)return false;
        resetEvidence();uint64_t cursor=0;if(!sample(cursor))return fail(HdaDellFaultStage::Service,why_);
        return reclaim(cursor);
    }
    // Terminal fault, including uncertain external codec writes. This first
    // attempts to mute and stop the controller, then quarantines its resources.
    // On an already failed queue it abandons any pending recovery.
    bool abort(){if(failed_){abandonRecovery();return false;}resetEvidence();return fail(HdaDellFaultStage::Abort,HdaDellFaultReason::ExternalAbort);}
    // RUN/BME stop is not descriptor retirement. The owner must still reset
    // and retire the stream before freeing pages or relinquishing ownership.
    bool shutdown(){
        if(!initialized_ || failed_ || closed_)return false;
        const bool muted=io_.mute(),stopped=io_.stop();
        if(!muted || !stopped){if(diagnostics_.enabled)add(diagnostics_.faults);
            HdaDellQueueFault f{};f.recorded=true;f.ordinal=__atomic_add_fetch(&faultCount_,uint64_t(1),__ATOMIC_RELAXED);f.stage=HdaDellFaultStage::Shutdown;
            f.reason=HdaDellFaultReason::ShutdownStop;f.epoch=clock_.epoch;f.muted=muted;f.stopped=stopped;
            f.rawCompleted=clock_.RawCompleted();f.submitted=clock_.submitted;f.completed=clock_.completed;f.cleared=cleared_;
            capture(f.afterStop);f.atNs=io_.nowNs();last_=f;if(!first_.recorded)first_=f;
            failed_=true;clock_.Fault();io_.quarantine();return false;}
        initialized_=false;closed_=true;if(diagnostics_.enabled)add(diagnostics_.stops);
        if(clock_.Pending())clock_.Fault(); // outstanding receipts were cancelled
        else clock_.running=false;
        return true;
    }
    // The owner still has to stop and retire its prepared descriptor, even
    // when no submit ever started this queue. Acknowledge that separately.
    bool closeBeforeStart(){
        if(initialized_ || failed_ || closed_)return false;
        clock_.Fault();closed_=true;return true;
    }
    // Call only after a verified clean stop, descriptor retirement and a
    // successful owner reprepare. Preserve the terminal epoch until this
    // explicit acknowledgement; uncertain failures may never use this path.
    bool restartAfterCleanStop(uint64_t expectedEpoch){
        if(!closed_ || initialized_ || failed_ || expectedEpoch!=clock_.epoch ||
           expectedEpoch==~uint64_t(0))return false;
        clock_.Fault();if(!clock_.Restart(expectedEpoch))return false;
        cleared_=lastMovementNs_=0;hasPublished_=false;closed_=false;return true;
    }
    // Opt-in: let fail() leave an admitted recovery candidate muted, stopped
    // and NOT quarantined, so the owner can retire and reprepare it. The
    // caller must then either recoverAfterVerifiedFault() or abandonRecovery().
    void allowRecovery(bool allowed){allowRecovery_=allowed;}
    bool recoverable()const{return failed_ && recoverable_;}
    void abandonRecovery(){if(recoverable_){recoverable_=false;last_.recoverable=false;io_.quarantine();}}
    // Call only after the owner verified retirement and reprepare of the
    // stream left stopped by an admitted fault. One use per fault; the
    // terminal epoch is acknowledged exactly as after a clean stop.
    bool recoverAfterVerifiedFault(uint64_t expectedEpoch){
        if(!failed_ || !recoverable_ || closed_ || expectedEpoch!=clock_.epoch || expectedEpoch==~uint64_t(0))return false;
        recoverable_=false;if(!clock_.Restart(expectedEpoch)){io_.quarantine();return false;}
        failed_=initialized_=false;cleared_=lastMovementNs_=0;hasPublished_=false;why_=HdaDellFaultReason::None;return true;
    }
    uint64_t faultCount()const{return __atomic_load_n(&faultCount_,__ATOMIC_RELAXED);}
    const HdaDellQueueFault& firstFault()const{return first_;}
    const HdaDellQueueFault& lastFault()const{return last_;}
    HdaDellReceipt submit(const void* samples,uint32_t bytes){
        HdaDellReceipt r{clock_.epoch,clock_.submitted,0,0};
        if(!samples || !bytes || (bytes&3)){r.error=-22;return r;}
        if(!initialized_ || failed_){r.error=-32;return r;}
        if(!service()){r.error=-32;return r;}
        resetEvidence();
        const uint64_t cursor=clock_.RawCompleted();
        if(cursor>~uint64_t(0)-guard_-maxBurst_-2*line_ || cleared_>~uint64_t(0)-Ring){fail(HdaDellFaultStage::Submit,HdaDellFaultReason::CursorOverflow);r.error=-32;return r;}
        // Leave a whole expected fetch burst between this sample and the
        // publication guard; the publisher samples again before copying.
        const uint64_t minBegin=(cursor+guard_+maxBurst_+2*line_-1)&~uint64_t(line_-1);
        uint64_t begin=clock_.submitted;if(begin<minBegin)begin=minBegin;
        const uint64_t clearedThrough=cleared_+Ring;
        if(cursor>~uint64_t(0)-Ring){fail(HdaDellFaultStage::Submit,HdaDellFaultReason::CursorOverflow);r.error=-32;return r;}
        uint64_t limit=cursor+Ring-guard_-line_;if(limit>clearedThrough)limit=clearedThrough;
        if(begin>=limit){if(diagnostics_.enabled)add(diagnostics_.backpressure);r.begin=begin;return r;} // ordinary backpressure
        uint64_t available=(limit-begin)&~uint64_t(3);if(bytes>available)bytes=uint32_t(available);
        if(!bytes){if(diagnostics_.enabled)add(diagnostics_.backpressure);r.begin=begin;return r;}
        if(begin!=clock_.submitted){
            const uint64_t gap=begin-clock_.submitted;
            if(!clock_.ReserveGap(begin)){fail(HdaDellFaultStage::Submit,HdaDellFaultReason::GapRejected);r.error=-32;return r;}
            if(diagnostics_.enabled){if(hasPublished_){add(diagnostics_.laterGapEvents);add(diagnostics_.laterGapBytes,gap);}
                else add(diagnostics_.startupGapBytes,gap);}
        }
        r.begin=begin;Publisher p{*this};
        // Reserve before writes so an uncertain exposure stays represented in
        // the terminal epoch even if DMA passes it during publication.
        if(!clock_.Accept(bytes)){fail(HdaDellFaultStage::Submit,HdaDellFaultReason::AcceptRejected);r.error=-32;return r;}
        const auto published=HdaStreamPublish(p,0,begin,samples,bytes,Ring,line_,guard_);
        if(published!=HdaStreamPublishResult::Published){
            if(diagnostics_.enabled)add(diagnostics_.publicationFailures);
            // An uncertain publication may already have reached the device.
            // Callers must not transparently retry it as if never exposed.
            if(published==HdaStreamPublishResult::Uncertain)r.accepted=bytes;
            fail(HdaDellFaultStage::Publish,publishReason(published));r.error=-32;return r;}
        r.accepted=bytes;if(diagnostics_.enabled)add(diagnostics_.publishedBytes,bytes);hasPublished_=true;return r;
    }
    const AudioDmaClock& progress()const{return clock_;}
    bool failed()const{return failed_;}
    HdaDellQueueDiagnostics diagnostics()const{return diagnostics_;}
};
}
