#pragma once
#include "hda_dell_stream_queue.h"
#include "../../kernel/src/audio.h"

namespace brook {
// Serialized mixer-facing adapter for an already prepared owner. This first
// version deliberately keeps the speaker muted; faults are terminal and the
// default specialization also treats STOP as final. The clean-restart variant
// is for hosted tests until physical admission is implemented. Lock methods must be
// process-context safe and must not disable interrupts over codec operations.
// Admission, AudioRegister() and object lifetime remain the caller's job.
struct HdaDellOutputRequest {bool authorized=false;uint32_t control=BROOK_AUDIO_MUTED;};
template<class Owner,class Lock,bool AllowCleanRestart=false,bool AllowAudible=false> class HdaDellAudioBridge {
    Owner& owner_;Lock& lock_;HdaDellStreamQueue<Owner> queue_;uint32_t capacity_;
    bool started_=false,closed_=false,retiredClean_=false,terminal_=false,hardwareMuted_=true,epochHasData_=false;
    struct Guard {Lock& lock;~Guard(){lock.unlock();}};
public:
    HdaDellAudioBridge(Owner& owner,Lock& lock,uint32_t line,uint32_t guard,uint32_t burst=1024,uint64_t clockUncertaintyNs=0,bool diagnostics=false):
        owner_(owner),lock_(lock),queue_(owner,line,guard,burst,clockUncertaintyNs,diagnostics),
        capacity_(line>=16 && line<=4096 && !(line&(line-1)) && !(4096%line) && burst &&
            burst<=guard && guard-burst>=line && guard<AudioDmaClock::RingBytes/4?AudioDmaClock::RingBytes:0){}
    HdaDellAudioBridge(const HdaDellAudioBridge&)=delete;
    HdaDellAudioBridge& operator=(const HdaDellAudioBridge&)=delete;
    AudioSubmitReceipt submit(const void* data,uint32_t bytes){
        if(!data || !bytes || (bytes&3))return {0,0,0,-22};
        if(!lock_.tryLock())return {};
        Guard guard{lock_};if(closed_ || !capacity_)return {queue_.progress().epoch,queue_.progress().submitted,0,-32};
        if(!started_){if(!queue_.start()){owner_.quarantine();closed_=true;return {queue_.progress().epoch,queue_.progress().submitted,0,-5};}started_=true;}
        const auto r=queue_.submit(data,bytes);if(r.accepted && !r.error)epochHasData_=true;
        return {r.epoch,r.begin,r.accepted,r.error};
    }
    int play(const void* data,uint32_t bytes,uint32_t rate,uint8_t channels,uint8_t bits){
        if(rate!=44100 || channels!=2 || bits!=16)return -22;
        const auto r=submit(data,bytes);return r.accepted?int(r.accepted):r.error;
    }
    bool progress(AudioDeviceProgress* result){
        if(!result)return false;lock_.lock();Guard guard{lock_};
        if(started_ && !closed_ && !queue_.failed())queue_.service();
        const auto& c=queue_.progress();*result={c.epoch,c.submitted,c.completed,capacity_,c.running,c.discontinuity || closed_};
        return capacity_!=0;
    }
    void service(){lock_.lock();Guard guard{lock_};if(started_ && !closed_ && !queue_.failed())queue_.service();}
    // The reader runs after taking the bridge lock so a stale queued control
    // snapshot cannot be applied after a stop/restart. Hardware changes are
    // deferred to process context; master volume remains software gain.
    template<class Read> bool reconcileOutput(Read read){
        if constexpr(!AllowAudible){(void)read;return false;}
        else {
            lock_.lock();Guard guard{lock_};if(closed_ || terminal_ || queue_.failed())return false;
            const auto wantsOutput=[](const HdaDellOutputRequest& request){return request.authorized &&
                !(request.control&BROOK_AUDIO_MUTED) && brook_audio_volume(request.control)!=0;};
            const bool audible=wantsOutput(read());
            if(!audible && !hardwareMuted_){if(!owner_.mute()){queue_.abort();closed_=true;return false;}hardwareMuted_=true;}
            else if(audible && started_ && epochHasData_ && queue_.progress().running && hardwareMuted_){
                if(!owner_.unmute()){queue_.abort();closed_=true;return false;}hardwareMuted_=false;
                // If a mute raced with the codec writes, apply it before
                // releasing the bridge lock. Later changes are reconciled at
                // the next service opportunity.
                if(!wantsOutput(read())){if(!owner_.mute()){queue_.abort();closed_=true;return false;}hardwareMuted_=true;}
            }
            return true;
        }
    }
    bool isPlaying(){lock_.lock();Guard guard{lock_};return started_ && !closed_ && !queue_.failed() && queue_.progress().Pending()!=0;}
    uint32_t pending(){lock_.lock();Guard guard{lock_};return closed_?0:queue_.progress().Pending();}
    // Private clock for the OSS drain watchdog; serialized with DMA sampling.
    // It does not expose or enable a general-purpose kernel clock.
    uint64_t drainClockNs(){lock_.lock();Guard guard{lock_};
        return !terminal_ && !closed_ && !queue_.failed()?owner_.nowNs():0;}
    HdaDellQueueDiagnostics diagnostics(){lock_.lock();Guard guard{lock_};return queue_.diagnostics();}
    // A clean close still requires descriptor retirement; uncertain failures
    // are quarantined and cannot be restarted by either specialization.
    bool stop(){
        lock_.lock();Guard guard{lock_};if(closed_)return false;closed_=true;
        if(queue_.failed())return false;
        if(started_){if(!queue_.shutdown())return false;}
        else {const bool muted=owner_.mute(),stopped=owner_.stop();if(!muted || !stopped){owner_.quarantine();return false;}}
        if constexpr(AllowCleanRestart){
            if(!owner_.retireRetained()){owner_.quarantine();return false;}
            if(!started_ && !queue_.closeBeforeStart()){owner_.quarantine();return false;}
            retiredClean_=true;return true;
        } else return owner_.retire();
    }
    bool restart(uint64_t expectedEpoch){
        if constexpr(!AllowCleanRestart){(void)expectedEpoch;return false;}
        else {
            lock_.lock();Guard guard{lock_};
            if(terminal_ || !closed_ || !retiredClean_ || queue_.failed() || expectedEpoch!=queue_.progress().epoch || expectedEpoch==~uint64_t(0))return false;
            retiredClean_=false; // an attempted reprepare can change hardware; never retry on failure
            if(!owner_.prepareRetained()){owner_.quarantine();return false;}
            if(!queue_.restartAfterCleanStop(expectedEpoch)){owner_.quarantine();return false;}
            started_=closed_=false;hardwareMuted_=true;epochHasData_=false;return true;
        }
    }
    // Irreversible shutdown shares the same lock as restart and submit. Even
    // a restart that raced with shutdown cannot reopen the controller later.
    bool shutdown(){
        lock_.lock();Guard guard{lock_};if(terminal_)return false;terminal_=true;
        if(closed_){if(!retiredClean_ || queue_.failed())return false;retiredClean_=false;return owner_.retire();}
        closed_=true;if(queue_.failed())return false;
        if(started_){if(!queue_.shutdown())return false;}
        else {const bool muted=owner_.mute(),stopped=owner_.stop();if(!muted || !stopped){owner_.quarantine();return false;}}
        return owner_.retire();
    }
};
}
