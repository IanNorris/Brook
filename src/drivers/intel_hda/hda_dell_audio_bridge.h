#pragma once
#include "hda_dell_stream_queue.h"
#include "../../kernel/src/audio.h"

namespace brook {
// Serialized mixer-facing adapter for an already prepared owner. This first
// version deliberately keeps the speaker muted and treats STOP/fault as final;
// it is not registered on real hardware. Lock::lock/tryLock/unlock must be
// process-context safe and must not disable interrupts over codec operations.
// Admission, AudioRegister() and object lifetime remain the caller's job.
template<class Owner,class Lock> class HdaDellAudioBridge {
    Owner& owner_;Lock& lock_;HdaDellStreamQueue<Owner> queue_;uint32_t capacity_;
    bool started_=false,closed_=false;
    struct Guard {Lock& lock;~Guard(){lock.unlock();}};
public:
    HdaDellAudioBridge(Owner& owner,Lock& lock,uint32_t line,uint32_t guard,uint32_t burst=1024):
        owner_(owner),lock_(lock),queue_(owner,line,guard,burst),
        capacity_(line>=16 && line<=4096 && !(line&(line-1)) && !(4096%line) && burst &&
            burst<=guard && guard-burst>=line && guard<AudioDmaClock::RingBytes/4?AudioDmaClock::RingBytes:0){}
    HdaDellAudioBridge(const HdaDellAudioBridge&)=delete;
    HdaDellAudioBridge& operator=(const HdaDellAudioBridge&)=delete;
    AudioSubmitReceipt submit(const void* data,uint32_t bytes){
        if(!data || !bytes || (bytes&3))return {0,0,0,-22};
        if(!lock_.tryLock())return {};
        Guard guard{lock_};if(closed_ || !capacity_)return {queue_.progress().epoch,queue_.progress().submitted,0,-32};
        if(!started_){if(!queue_.start()){owner_.quarantine();closed_=true;return {queue_.progress().epoch,queue_.progress().submitted,0,-5};}started_=true;}
        const auto r=queue_.submit(data,bytes);return {r.epoch,r.begin,r.accepted,r.error};
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
    bool isPlaying(){lock_.lock();Guard guard{lock_};return started_ && !closed_ && !queue_.failed() && queue_.progress().Pending()!=0;}
    uint32_t pending(){lock_.lock();Guard guard{lock_};return closed_?0:queue_.progress().Pending();}
    // A clean close still requires descriptor retirement; uncertain failures
    // are quarantined and cannot be restarted by this one-shot adapter.
    bool stop(){
        lock_.lock();Guard guard{lock_};if(closed_)return false;closed_=true;
        if(queue_.failed())return false;
        if(started_){if(!queue_.shutdown())return false;}
        else {const bool muted=owner_.mute(),stopped=owner_.stop();if(!muted || !stopped){owner_.quarantine();return false;}}
        return owner_.retire();
    }
    bool restart(uint64_t){return false;} // terminal until a fresh owner is implemented
};
}
